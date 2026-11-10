#include "api/download.h"

#include <cstddef>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>

#include "api/auth.h"
#include "api/respond.h"
#include "db/blobs.h"
#include "db/files.h"
#include "log.h"
#include "storage/blob.h"
#include "util/range.h"

namespace api_download {

namespace {

// RFC 5987 attr-char：其余字节一律百分号转义，中文名也能直接进响应头。
bool is_attr_char(unsigned char c) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
        return true;
    }
    const std::string extra = "!#$&+-.^_`|~";
    return extra.find(static_cast<char>(c)) != std::string::npos;
}

std::string rfc5987_encode(const std::string& text) {
    static const char* const kHex = "0123456789ABCDEF";
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (is_attr_char(c)) {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 0x0F];
        }
    }
    return out;
}

// 只给不支持 filename* 的老客户端用的 ASCII 兜底名。
std::string ascii_fallback(const std::string& name) {
    std::string out;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        if (c < 0x20 || c >= 0x7F || c == '"' || c == '\\') {
            out += '_';
        } else {
            out += static_cast<char>(c);
        }
    }
    if (out.empty()) {
        out = "download";
    }
    return out;
}

bool parse_id(const std::string& text, long long& out) {
    if (text.empty() || text.size() > 18) {
        return false;
    }
    long long value = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + (c - '0');
    }
    out = value;
    return true;
}

// 以只读方式打开 blob 并把 [begin, begin+len) 流式写入连接。
bool serve_file(const std::string& path, long long begin, long long len,
                httplib::DataSink& sink) {
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) {
        return false;
    }
    in.seekg(static_cast<std::streamoff>(begin), std::ios::beg);
    if (!in) {
        return false;
    }

    std::vector<char> buffer(128 * 1024);
    long long remaining = len;
    while (remaining > 0) {
        const std::size_t want =
            remaining < static_cast<long long>(buffer.size()) ? static_cast<std::size_t>(remaining)
                                                              : buffer.size();
        in.read(&buffer[0], static_cast<std::streamsize>(want));
        const std::streamsize got = in.gcount();
        if (got <= 0) {
            return false;  // 实际内容比声明的长度短，不能静默截断
        }
        if (!sink.write(buffer.data(), static_cast<std::size_t>(got))) {
            return false;
        }
        remaining -= static_cast<long long>(got);
    }
    return true;
}

void handle_content(const httplib::Request& req, httplib::Response& res, const Config& cfg) {
    long long user_id = 0;
    if (!auth::resolve(req, user_id)) {
        api::json_error(res, 401, "unauthorized", "登录已过期，请重新登录");
        return;
    }

    long long file_id = 0;
    if (req.matches.size() < 2 || !parse_id(req.matches[1], file_id)) {
        api::json_error(res, 404, "not_found", "文件不存在");
        return;
    }

    db::files::Row row;
    if (!db::files::find_file(user_id, file_id, row)) {
        // 不属于该账户、在回收站、或本来就是文件夹：一律按不存在处理。
        api::json_error(res, 404, "not_found", "文件不存在");
        return;
    }

    db::blobs::Info blob;
    if (!db::blobs::find_by_id(row.blob_id, blob)) {
        log_error("文件条目找不到对应内容记录：id=" + std::to_string(file_id));
        api::json_error(res, 404, "not_found", "文件内容不存在");
        return;
    }

    const std::string path = storage::blob_path(cfg.data_dir, blob.sha256);
    struct stat st;
    if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        log_error("内容文件缺失：" + blob.sha256);
        api::json_error(res, 404, "not_found", "文件内容不存在");
        return;
    }
    const long long size = static_cast<long long>(st.st_size);

    res.set_header("Accept-Ranges", "bytes");
    res.set_header("ETag", "\"" + blob.sha256 + "\"");
    res.set_header("X-Content-SHA256", blob.sha256);
    res.set_header("Content-Disposition",
                   "attachment; filename=\"" + ascii_fallback(row.name) + "\"; filename*=UTF-8''" +
                       rfc5987_encode(row.name));

    long long start = 0;
    long long length = size;

    if (req.has_header("Range")) {
        const util::RangeSpec spec = util::parse_range(req.get_header_value("Range"), size);
        if (!spec.valid) {
            res.set_header("Content-Range", "bytes */" + std::to_string(size));
            api::json_error(res, 416, "range_not_satisfiable", "请求的区间无法满足");
            return;
        }
        start = spec.start;
        length = spec.last - spec.start + 1;
        res.status = 206;
        res.set_header("Content-Range", "bytes " + std::to_string(spec.start) + "-" +
                                            std::to_string(spec.last) + "/" +
                                            std::to_string(size));
    }

    res.set_content_provider(static_cast<std::size_t>(length), "application/octet-stream",
                             [path, start](std::size_t offset, std::size_t want,
                                           httplib::DataSink& sink) {
                                 // httplib 传进来的是"响应体内偏移 + 剩余字节数"，
                                 // 换算成文件内偏移后分块读到连接里。
                                 const long long begin = start + static_cast<long long>(offset);
                                 return serve_file(path, begin, static_cast<long long>(want), sink);
                             });
}

}  // namespace

void register_routes(httplib::Server& svr, const Config& cfg) {
    svr.Get(R"(/api/files/([0-9]+)/content)",
            [&cfg](const httplib::Request& req, httplib::Response& res) {
                handle_content(req, res, cfg);
            });
}

}  // namespace api_download
