#pragma once

#include <string>

// 登录会话访问：库里只存 token 的 SHA-256，不存 token 原文。
namespace db {

bool create_session(long long user_id, const std::string& token_hash, long long expires_at,
                    std::string& err);

// 查询会话；不存在或已过期返回 false。
bool find_session(const std::string& token_hash, long long& out_user_id, long long& out_expires_at,
                  std::string& err);

bool delete_session(const std::string& token_hash, std::string& err);

// 清理所有已过期会话，返回删除条数；失败返回 -1。
int delete_expired_sessions();

}  // namespace db
