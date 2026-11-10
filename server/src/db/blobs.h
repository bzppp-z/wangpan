#pragma once

#include <string>

// blobs 表访问：内容寻址存储的元数据，同一内容只占一份磁盘空间。
namespace db {
namespace blobs {

// 按 sha256 取已有 blob；不存在则插入（refcount = 0）。
// 并发插入同一 sha 时依赖唯一索引，冲突后回读已有行。
bool get_or_create(const std::string& sha256, long long size, long long& out_id, std::string& err);

// 引用计数 +1（每次新建文件条目调用一次）。
bool incref(long long blob_id, std::string& err);

}  // namespace blobs
}  // namespace db
