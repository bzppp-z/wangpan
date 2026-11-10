#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "storage/blob.h"
#include "util/crypto.h"

static int g_fail = 0;

static void expect(bool ok, const char* name) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) {
        ++g_fail;
    }
}

static bool write_raw(const std::string& path, const std::string& bytes) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        return false;
    }
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    std::fclose(f);
    return ok;
}

static long long file_size(const std::string& path) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) {
        return -1;
    }
    return static_cast<long long>(st.st_size);
}

int main() {
    std::string err;

    char tmpl[] = "/tmp/wp_storage_XXXXXX";
    char* created = ::mkdtemp(tmpl);
    if (created == nullptr) {
        std::fprintf(stderr, "mkdtemp failed\n");
        return 1;
    }
    const std::string base(created);
    const std::string session = base + "/sess";
    const std::string data_dir = base + "/data";
    ::mkdir(session.c_str(), 0700);
    ::mkdir(data_dir.c_str(), 0700);

    const std::int64_t chunk_size = 4 * 1024 * 1024;
    const std::string chunk0(static_cast<std::size_t>(chunk_size), 'A');
    const std::string chunk1(1024 * 1024, 'B');
    const std::int64_t total = chunk_size + static_cast<std::int64_t>(chunk1.size());

    expect(storage::write_chunk(session, 0, chunk0, chunk_size, err), "write chunk 0");
    expect(storage::write_chunk(session, 1, chunk1, static_cast<std::int64_t>(chunk1.size()), err),
           "write chunk 1");

    // 长度不符必须拒绝，且不留下分片文件。
    expect(!storage::write_chunk(session, 2, std::string(10, 'x'), chunk_size, err),
           "reject chunk with wrong length");
    expect(file_size(session + "/2.chunk") < 0, "rejected chunk left no file");

    std::vector<int> received;
    expect(storage::scan_received(session, 2, chunk_size, total, received, err), "scan received");
    expect(received == std::vector<int>({0, 1}), "received = {0,1}");

    // 大小不对的分片视为未收：把第 1 片截断成 100 字节。
    expect(write_raw(session + "/1.chunk", std::string(100, 'B')), "truncate chunk 1");
    expect(storage::scan_received(session, 2, chunk_size, total, received, err),
           "scan ignores bad chunk");
    expect(received == std::vector<int>({0}), "truncated chunk not received");

    // 客户端续传补回第 1 片后恢复。
    expect(storage::write_chunk(session, 1, chunk1, static_cast<std::int64_t>(chunk1.size()), err),
           "rewrite chunk 1");
    expect(storage::scan_received(session, 2, chunk_size, total, received, err), "rescan received");
    expect(received == std::vector<int>({0, 1}), "received back to {0,1}");

    const std::string merged = base + "/merged.bin";
    std::string sha;
    expect(storage::merge_and_hash(session, 2, chunk_size, total, merged, sha, err),
           "merge and hash");
    expect(sha == crypto::sha256_hex(chunk0 + chunk1), "sha matches full content");
    expect(file_size(merged) == total, "merged size is 5 MiB");

    expect(storage::commit_blob(merged, data_dir, sha, err), "commit blob");
    expect(file_size(storage::blob_path(data_dir, sha)) == total, "blob stored with right size");

    // 同 sha 再提交一次（模拟两个账户上传同一内容）：不报错，不破坏已有文件。
    const std::string merged2 = base + "/merged2.bin";
    std::string sha2;
    expect(storage::merge_and_hash(session, 2, chunk_size, total, merged2, sha2, err),
           "merge again");
    expect(sha2 == sha, "same content same sha");
    expect(storage::commit_blob(merged2, data_dir, sha2, err), "commit same blob again");
    expect(file_size(storage::blob_path(data_dir, sha)) == total, "blob intact after re-commit");

    // 缺片时合并必须失败，且不留半截输出文件。
    ::unlink((session + "/0.chunk").c_str());
    std::string bad_sha;
    expect(!storage::merge_and_hash(session, 2, chunk_size, total, base + "/bad.bin", bad_sha, err),
           "merge fails on missing chunk");
    expect(file_size(base + "/bad.bin") < 0, "failed merge removed partial output");

    storage::remove_tree(base);
    expect(file_size(base) < 0, "remove_tree cleans up");

    std::printf("storage: %s (%d failed)\n", g_fail ? "FAILED" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
