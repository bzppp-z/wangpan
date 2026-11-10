#pragma once

#include <string>

// 用户表访问。
namespace db {

// 插入用户；用户名重复时返回 false 且 err 为"用户名已存在"。
bool create_user(const std::string& username, const std::string& salt_hex,
                 const std::string& hash_hex, long long quota_bytes, long long& out_id,
                 std::string& err);

// 按用户名查询；不存在返回 false，err 为空。
bool find_user_by_name(const std::string& username, long long& out_id, std::string& out_salt,
                       std::string& out_hash, long long& out_quota, std::string& err);

// 该用户所有 kind='file' 记录的 size 之和（含回收站内文件，符合 PRD R9）。
long long used_bytes(long long user_id);

}  // namespace db
