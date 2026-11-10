#include "storage/blob.h"

#include <openssl/evp.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace storage {

namespace {

const char* kHexDigits = "0123456789abcdef";

std::string to_hex(const unsigned char* data, unsigned int len) {
    std::string out;
    out.reserve(static_cast<std::size_t>(len) * 2);
    for (unsigned int i = 0; i < len; ++i) {
        out.push_back(kHexDigits[data[i] >> 4]);
        out.push_back(kHexDigits[data[i] & 0x0F]);
    }
    return out;
}

std::string chunk_path(const std::string& session_dir, int index) {
    return session_dir + "/" + std::to_string(index) + ".chunk";
}

// 第 index 片的应有长度；越界返回 -1。
std::int64_t chunk_len(int index, std::int64_t chunk_size, std::int64_t total_size) {
    if (index < 0 || chunk_size <= 0 || total_size < 0) {
        return -1;
    }
    const std::int64_t offset = static_cast<std::int64_t>(index) * chunk_size;
    if (offset >= total_size) {
        return total_size == offset ? 0 : -1;
    }
    const std::int64_t left = total_size - offset;
    return left < chunk_size ? left : chunk_size;
}

bool make_dir(const std::string& path) {
    if (::mkdir(path.c_str(), 0755) == 0) {
        return true;
    }
    if (errno != EEXIST) {
        return false;
    }
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// 逐级创建目录，类似 mkdir -p。
bool ensure_dir(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    std::string current;
    std::size_t i = 0;
    if (path[0] == '/') {
        current = "/";
        i = 1;
    }
    while (i <= path.size()) {
        const std::size_t slash = path.find('/', i);
        const std::string part = path.substr(i, slash == std::string::npos ? std::string::npos
                                                                           : slash - i);
        if (!part.empty()) {
            if (!current.empty() && current.back() != '/') {
                current.push_back('/');
            }
            current += part;
            if (!make_dir(current)) {
                return false;
            }
        }
        if (slash == std::string::npos) {
            break;
        }
        i = slash + 1;
    }
    return true;
}

std::string parent_dir(const std::string& path) {
    const std::size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) {
        return ".";
    }
    return slash == 0 ? "/" : path.substr(0, slash);
}

// 把 src 完整复制到 dst（用于跨文件系统兜底）。
bool copy_file(const std::string& src, const std::string& dst) {
    FILE* in = std::fopen(src.c_str(), "rb");
    if (in == nullptr) {
        return false;
    }
    FILE* out = std::fopen(dst.c_str(), "wb");
    if (out == nullptr) {
        std::fclose(in);
        return false;
    }
    bool ok = true;
    char buf[1 << 20];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), in)) > 0) {
        if (std::fwrite(buf, 1, n, out) != n) {
            ok = false;
            break;
        }
    }
    if (std::ferror(in) != 0) {
        ok = false;
    }
    if (std::fflush(out) != 0) {
        ok = false;
    }
    std::fclose(in);
    if (std::fclose(out) != 0) {
        ok = false;
    }
    if (!ok) {
        ::unlink(dst.c_str());
    }
    return ok;
}

}  // namespace

std::string blob_path(const std::string& data_dir, const std::string& sha256) {
    if (sha256.size() < 4) {
        return std::string();
    }
    return data_dir + "/blob/" + sha256.substr(0, 2) + "/" + sha256.substr(2, 2) + "/" + sha256;
}

bool write_chunk(const std::string& session_dir, int index, const std::string& bytes,
                 std::int64_t expected_len, std::string& err) {
    err.clear();
    if (index < 0) {
        err = "分片序号非法";
        return false;
    }
    if (expected_len < 0 || static_cast<std::int64_t>(bytes.size()) != expected_len) {
        err = "分片长度不符";
        return false;
    }
    if (!ensure_dir(session_dir)) {
        err = "无法创建会话目录";
        return false;
    }

    const std::string final_path = chunk_path(session_dir, index);
    const std::string tmp_path = final_path + ".tmp";
    const int fd = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        err = "无法写入分片文件";
        return false;
    }

    const char* pos = bytes.data();
    std::size_t left = bytes.size();
    while (left > 0) {
        const ssize_t n = ::write(fd, pos, left);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            ::close(fd);
            ::unlink(tmp_path.c_str());
            err = "写分片失败";
            return false;
        }
        pos += n;
        left -= static_cast<std::size_t>(n);
    }
    if (::fsync(fd) != 0) {
        ::close(fd);
        ::unlink(tmp_path.c_str());
        err = "分片落盘失败";
        return false;
    }
    if (::close(fd) != 0) {
        ::unlink(tmp_path.c_str());
        err = "分片落盘失败";
        return false;
    }
    // 原子改名：只有完整写入并 fsync 过的分片才会以 <index>.chunk 出现。
    if (::rename(tmp_path.c_str(), final_path.c_str()) != 0) {
        ::unlink(tmp_path.c_str());
        err = "分片落位失败";
        return false;
    }
    return true;
}

bool scan_received(const std::string& session_dir, int chunk_count, std::int64_t chunk_size,
                   std::int64_t total_size, std::vector<int>& out_indices, std::string& err) {
    err.clear();
    out_indices.clear();
    if (chunk_count < 0) {
        err = "分片数量非法";
        return false;
    }
    for (int i = 0; i < chunk_count; ++i) {
        const std::int64_t expected = chunk_len(i, chunk_size, total_size);
        if (expected < 0) {
            err = "分片长度非法";
            return false;
        }
        struct stat st;
        if (::stat(chunk_path(session_dir, i).c_str(), &st) != 0) {
            if (errno == ENOENT) {
                continue;
            }
            err = "读取分片信息失败";
            return false;
        }
        if (S_ISREG(st.st_mode) && static_cast<std::int64_t>(st.st_size) == expected) {
            out_indices.push_back(i);
        }
    }
    return true;
}

bool merge_and_hash(const std::string& session_dir, int chunk_count, std::int64_t chunk_size,
                    std::int64_t total_size, const std::string& out_path,
                    std::string& out_sha256, std::string& err) {
    err.clear();
    out_sha256.clear();
    if (chunk_count < 0 || chunk_size <= 0 || total_size < 0) {
        err = "合并参数非法";
        return false;
    }

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (ctx == nullptr || EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
        EVP_MD_CTX_free(ctx);
        err = "初始化哈希失败";
        return false;
    }
    if (!ensure_dir(parent_dir(out_path))) {
        EVP_MD_CTX_free(ctx);
        err = "无法创建输出目录";
        return false;
    }
    FILE* out = std::fopen(out_path.c_str(), "wb");
    if (out == nullptr) {
        EVP_MD_CTX_free(ctx);
        err = "无法创建合并文件";
        return false;
    }

    bool ok = true;
    std::vector<char> buf(1 << 20);
    std::int64_t written = 0;
    for (int i = 0; i < chunk_count && ok; ++i) {
        const std::int64_t expected = chunk_len(i, chunk_size, total_size);
        struct stat st;
        if (expected < 0 || ::stat(chunk_path(session_dir, i).c_str(), &st) != 0 ||
            !S_ISREG(st.st_mode) || static_cast<std::int64_t>(st.st_size) != expected) {
            err = "分片 " + std::to_string(i) + " 缺失或长度不符";
            ok = false;
            break;
        }
        FILE* in = std::fopen(chunk_path(session_dir, i).c_str(), "rb");
        if (in == nullptr) {
            err = "无法读取分片 " + std::to_string(i);
            ok = false;
            break;
        }
        std::int64_t left = expected;
        while (left > 0) {
            const std::size_t want = static_cast<std::size_t>(
                left < static_cast<std::int64_t>(buf.size()) ? left
                                                             : static_cast<std::int64_t>(buf.size()));
            const std::size_t got = std::fread(buf.data(), 1, want, in);
            if (got != want) {
                err = "分片 " + std::to_string(i) + " 读取不完整";
                ok = false;
                break;
            }
            if (EVP_DigestUpdate(ctx, buf.data(), got) != 1) {
                err = "计算哈希失败";
                ok = false;
                break;
            }
            if (std::fwrite(buf.data(), 1, got, out) != got) {
                err = "写合并文件失败";
                ok = false;
                break;
            }
            left -= static_cast<std::int64_t>(got);
            written += static_cast<std::int64_t>(got);
        }
        std::fclose(in);
    }
    if (ok && written != total_size) {
        err = "合并后长度不符";
        ok = false;
    }

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    if (ok && EVP_DigestFinal_ex(ctx, digest, &digest_len) != 1) {
        err = "计算哈希失败";
        ok = false;
    }
    EVP_MD_CTX_free(ctx);
    if (std::fflush(out) != 0) {
        err = "写合并文件失败";
        ok = false;
    }
    if (std::fclose(out) != 0) {
        err = "写合并文件失败";
        ok = false;
    }
    if (!ok) {
        ::unlink(out_path.c_str());
        return false;
    }
    out_sha256 = to_hex(digest, digest_len);
    return true;
}

bool commit_blob(const std::string& temp_path, const std::string& data_dir,
                 const std::string& sha256, std::string& err) {
    err.clear();
    const std::string target = blob_path(data_dir, sha256);
    if (target.empty()) {
        err = "内容哈希非法";
        return false;
    }

    struct stat temp_st;
    if (::stat(temp_path.c_str(), &temp_st) != 0 || !S_ISREG(temp_st.st_mode)) {
        err = "合并文件不存在";
        return false;
    }
    struct stat target_st;
    if (::stat(target.c_str(), &target_st) == 0) {
        // 同一内容只存一份：已有且大小一致时直接丢弃临时文件。
        if (target_st.st_size == temp_st.st_size) {
            if (::unlink(temp_path.c_str()) != 0 && errno != ENOENT) {
                err = "清理临时文件失败";
                return false;
            }
            return true;
        }
        err = "已存在同哈希但大小不同的文件";
        return false;
    }
    if (!ensure_dir(parent_dir(target))) {
        err = "无法创建内容目录";
        return false;
    }
    if (::rename(temp_path.c_str(), target.c_str()) == 0) {
        return true;
    }
    if (errno != EXDEV) {
        err = "内容落位失败";
        return false;
    }

    // 跨文件系统：先复制到目标同目录，再原子改名，避免出现半截 blob。
    const std::string staging = target + ".importing";
    if (!copy_file(temp_path, staging)) {
        err = "内容复制失败";
        return false;
    }
    if (::rename(staging.c_str(), target.c_str()) != 0) {
        ::unlink(staging.c_str());
        err = "内容落位失败";
        return false;
    }
    ::unlink(temp_path.c_str());
    return true;
}

bool remove_tree(const std::string& path) {
    struct stat st;
    if (::lstat(path.c_str(), &st) != 0) {
        return errno == ENOENT;
    }
    if (!S_ISDIR(st.st_mode)) {
        return ::unlink(path.c_str()) == 0;
    }
    DIR* dir = ::opendir(path.c_str());
    if (dir == nullptr) {
        return false;
    }
    bool ok = true;
    while (struct dirent* entry = ::readdir(dir)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        if (!remove_tree(path + "/" + name)) {
            ok = false;
        }
    }
    ::closedir(dir);
    if (::rmdir(path.c_str()) != 0) {
        ok = false;
    }
    return ok;
}

}  // namespace storage
