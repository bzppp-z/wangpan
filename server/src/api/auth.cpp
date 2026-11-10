#include "api/auth.h"

#include <cstddef>
#include <string>

#include "api/respond.h"
#include "db/db.h"
#include "db/sessions.h"
#include "db/users.h"
#include "log.h"
#include "nlohmann/json.hpp"
#include "util/crypto.h"

namespace auth {

namespace {

const std::size_t kMaxBodyBytes = 16 * 1024;  // PRD R4：请求体上限 16 KB
const std::size_t kSaltBytes = 32;
const std::size_t kTokenBytes = 32;

// 由 register_routes 注入，避免每个 handler 都带一个配额参数。
long long g_quota_bytes = 5LL * 1024 * 1024 * 1024;

bool valid_username(const std::string& name) {
    if (name.size() < 3 || name.size() > 20) {
        return false;
    }
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                             (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

bool valid_password(const std::string& password) {
    return password.size() >= 8 && password.size() <= 64;
}

// 解析 {"username","password"}；失败时已写好响应。
bool parse_credentials(const httplib::Request& req, std::string& out_user, std::string& out_pass,
                       httplib::Response& res) {
    if (req.body.size() > kMaxBodyBytes) {
        api::json_error(res, 413, "payload_too_large", "请求内容过大");
        return false;
    }
    const nlohmann::json body = nlohmann::json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.is_object()) {
        api::json_error(res, 400, "invalid_request", "请求体不是合法 JSON");
        return false;
    }
    if (!body.contains("username") || !body.contains("password") ||
        !body["username"].is_string() || !body["password"].is_string()) {
        api::json_error(res, 400, "invalid_request", "缺少用户名或密码");
        return false;
    }
    out_user = body["username"].get<std::string>();
    out_pass = body["password"].get<std::string>();
    return true;
}

// 生成 token 并落库；库里只存 SHA-256。
bool issue_session(long long user_id, std::string& out_token, long long& out_expires_at,
                   std::string& err) {
    out_token = crypto::random_hex(kTokenBytes);
    if (out_token.size() != kTokenBytes * 2) {
        err = "生成会话 token 失败";
        return false;
    }
    out_expires_at = db::now_seconds() + kSessionTtlSeconds;
    return db::create_session(user_id, crypto::sha256_hex(out_token), out_expires_at, err);
}

void respond_session(httplib::Response& res, long long user_id, const std::string& token,
                     long long expires_at, int status) {
    nlohmann::json body;
    body["user_id"] = user_id;
    body["token"] = token;
    body["expires_at"] = expires_at;
    api::json_ok(res, body, status);
}

}  // namespace

std::string bearer_token(const httplib::Request& req) {
    const std::string header = req.get_header_value("Authorization");
    const std::string prefix = "Bearer ";
    if (header.size() <= prefix.size() ||
        header.compare(0, prefix.size(), prefix) != 0) {
        return std::string();
    }
    return header.substr(prefix.size());
}

bool resolve(const httplib::Request& req, long long& out_user_id) {
    out_user_id = 0;
    const std::string token = bearer_token(req);
    if (token.size() != kTokenBytes * 2) {
        return false;
    }
    long long expires_at = 0;
    std::string err;
    return db::find_session(crypto::sha256_hex(token), out_user_id, expires_at, err);
}

void handle_register(const httplib::Request& req, httplib::Response& res) {
    std::string username;
    std::string password;
    if (!parse_credentials(req, username, password, res)) {
        return;
    }
    if (!valid_username(username)) {
        api::json_error(res, 400, "invalid_username",
                        "用户名需 3–20 个字符，仅限字母、数字、下划线和短横线");
        return;
    }
    if (!valid_password(password)) {
        api::json_error(res, 400, "invalid_password", "密码需 8–64 个字符");
        return;
    }

    const std::string salt = crypto::random_hex(kSaltBytes);
    const std::string hash = crypto::pbkdf2_sha256(password, salt);
    if (salt.empty() || hash.empty()) {
        log_error("注册失败：密码散列计算异常");
        api::json_error(res, 500, "internal_error", "服务器内部错误");
        return;
    }

    long long user_id = 0;
    std::string err;
    if (!db::create_user(username, salt, hash, g_quota_bytes, user_id, err)) {
        if (err == "用户名已存在") {
            api::json_error(res, 409, "username_taken", "该用户名已被使用");
        } else {
            log_error("注册失败：" + err);
            api::json_error(res, 500, "internal_error", "服务器内部错误");
        }
        return;
    }

    std::string token;
    long long expires_at = 0;
    if (!issue_session(user_id, token, expires_at, err)) {
        log_error("注册后创建会话失败：" + err);
        api::json_error(res, 500, "internal_error", "服务器内部错误");
        return;
    }
    respond_session(res, user_id, token, expires_at, 201);
}

void handle_login(const httplib::Request& req, httplib::Response& res) {
    std::string username;
    std::string password;
    if (!parse_credentials(req, username, password, res)) {
        return;
    }

    long long user_id = 0;
    long long quota_bytes = 0;
    std::string salt;
    std::string hash;
    std::string err;
    const bool found = db::find_user_by_name(username, user_id, salt, hash, quota_bytes, err);
    if (!found && !err.empty()) {
        log_error("登录查询失败：" + err);
        api::json_error(res, 500, "internal_error", "服务器内部错误");
        return;
    }

    // 用户不存在时也走一次散列，避免用响应时间区分"用户是否存在"。
    const std::string candidate =
        crypto::pbkdf2_sha256(password, found ? salt : std::string(kSaltBytes * 2, '0'));
    if (!found || candidate.empty() || !crypto::constant_time_equal(candidate, hash)) {
        api::json_error(res, 401, "invalid_credentials", "账号或密码错误");
        return;
    }

    std::string token;
    long long expires_at = 0;
    if (!issue_session(user_id, token, expires_at, err)) {
        log_error("登录后创建会话失败：" + err);
        api::json_error(res, 500, "internal_error", "服务器内部错误");
        return;
    }
    respond_session(res, user_id, token, expires_at, 200);
}

void handle_logout(const httplib::Request& req, httplib::Response& res) {
    long long user_id = 0;
    if (!resolve(req, user_id)) {
        api::json_error(res, 401, "unauthorized", "登录已过期，请重新登录");
        return;
    }

    std::string err;
    if (!db::delete_session(crypto::sha256_hex(bearer_token(req)), err)) {
        log_error("登出失败：" + err);
        api::json_error(res, 500, "internal_error", "服务器内部错误");
        return;
    }
    res.status = 204;
}

void register_routes(httplib::Server& svr, long long quota_bytes) {
    g_quota_bytes = quota_bytes;
    svr.Post("/api/register", handle_register);
    svr.Post("/api/login", handle_login);
    svr.Post("/api/logout", handle_logout);
}

}  // namespace auth
