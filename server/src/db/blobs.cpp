#include "db/blobs.h"

namespace db {
namespace blobs {

// 桩实现：本提交只用于让 test_instant 可编译并「先红」。
bool get_or_create(const std::string& sha256, long long size, long long& out_id, std::string& err) {
    (void)sha256;
    (void)size;
    out_id = 0;
    err = "尚未实现";
    return false;
}

bool incref(long long blob_id, std::string& err) {
    (void)blob_id;
    err = "尚未实现";
    return false;
}

}  // namespace blobs
}  // namespace db
