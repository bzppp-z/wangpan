#pragma once

#include <sqlite3.h>

#include <string>

// 数据访问层统一入口：连接管理与建表。
// 所有函数都不抛异常，失败时把中文原因写入 err。
namespace db {

// 打开数据库。":memory:" 为内存库（测试用，跳过 WAL）。
bool open(const std::string& path, std::string& err);

// 关闭数据库；未打开时是空操作。
void close();

// 当前连接，未打开时为 nullptr。
sqlite3* handle();

// 幂等建表（内容见 db/schema.h）。
bool init_schema(std::string& err);

// 当前 Unix 秒级时间戳，供各表写入 created_at / updated_at / expires_at。
long long now_seconds();

}  // namespace db
