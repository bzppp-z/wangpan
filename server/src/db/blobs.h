#pragma once

#include <string>
#include <vector>

// blobs 表访问：内容寻址存储的元数据，同一内容只占一份磁盘空间。
namespace db {
namespace blobs {

struct Info {
    long long id = 0;
    long long size = 0;
    std::string sha256;
};

// 按 id 取已有 blob；不存在返回 false。
bool find_by_id(long long blob_id, Info& out);

// 按 sha256 取已有 blob；不存在则插入（refcount = 0）。
// 并发插入同一 sha 时依赖唯一索引，冲突后回读已有行。
bool get_or_create(const std::string& sha256, long long size, long long& out_id, std::string& err);

// 引用计数 +1（每次新建文件条目调用一次）。
bool incref(long long blob_id, std::string& err);

// 引用计数 -1；refcount 降到 0 时删除 blobs 行，并通过 out_sha 返回需要清理的磁盘内容。
// out_removed 表示这一行确实被删除了（调用方据此提交事务后删磁盘文件）。
bool decref_and_collect(long long blob_id, std::string& out_sha, bool& out_removed,
                        std::string& err);

// 删除所有 refcount <= 0 的孤儿 blob 行（启动清理用）。
// 返回对应的 sha 列表，由调用方在事务提交后删除磁盘文件。
bool remove_zero_ref(std::vector<std::string>& out_shas, std::string& err);

}  // namespace blobs
}  // namespace db
