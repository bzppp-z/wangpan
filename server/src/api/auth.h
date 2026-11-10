#pragma once

#include <string>

#include "httplib.h"

// 账户与会话接口：注册、登录、登出，以及受保护接口用的 Bearer 鉴权。
namespace auth {

// 登录会话有效期：7 天（PRD R11）。
const long long kSessionTtlSeconds = 7LL * 24 * 3600;

// 从请求头解析并校验 token；成功时写入 out_user_id 并返回 true。
bool resolve(const httplib::Request& req, long long& out_user_id);

// 从 Authorization 头取出 Bearer 后的原始 token；缺失或格式不对返回空串。
std::string bearer_token(const httplib::Request& req);

void handle_register(const httplib::Request& req, httplib::Response& res);
void handle_login(const httplib::Request& req, httplib::Response& res);
void handle_logout(const httplib::Request& req, httplib::Response& res);

// 注册 /api/register、/api/login、/api/logout。
void register_routes(httplib::Server& svr);

}  // namespace auth
