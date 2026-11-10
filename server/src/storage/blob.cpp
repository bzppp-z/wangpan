#include "storage/blob.h"

// 桩实现：本提交只用于让 test_storage 可编译并「先红」，
// 真实实现见同一任务的第二个提交。
namespace storage {

std::string blob_path(const std::string& data_dir, const std::string& sha256) {
    (void)data_dir;
    (void)sha256;
    return std::string();
}

bool write_chunk(const std::string& session_dir, int index, const std::string& bytes,
                 std::int64_t expected_len, std::string& err) {
    (void)session_dir;
    (void)index;
    (void)bytes;
    (void)expected_len;
    err = "尚未实现";
    return false;
}

bool scan_received(const std::string& session_dir, int chunk_count, std::int64_t chunk_size,
                   std::int64_t total_size, std::vector<int>& out_indices, std::string& err) {
    (void)session_dir;
    (void)chunk_count;
    (void)chunk_size;
    (void)total_size;
    (void)out_indices;
    err = "尚未实现";
    return false;
}

bool merge_and_hash(const std::string& session_dir, int chunk_count, std::int64_t chunk_size,
                    std::int64_t total_size, const std::string& out_path,
                    std::string& out_sha256, std::string& err) {
    (void)session_dir;
    (void)chunk_count;
    (void)chunk_size;
    (void)total_size;
    (void)out_path;
    (void)out_sha256;
    err = "尚未实现";
    return false;
}

bool commit_blob(const std::string& temp_path, const std::string& data_dir,
                 const std::string& sha256, std::string& err) {
    (void)temp_path;
    (void)data_dir;
    (void)sha256;
    err = "尚未实现";
    return false;
}

bool remove_tree(const std::string& path) {
    (void)path;
    return false;
}

}  // namespace storage
