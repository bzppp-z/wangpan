#include "api/manage.h"

#include <cstddef>
#include <string>

#include "api/auth.h"
#include "api/files.h"
#include "api/respond.h"
#include "db/files.h"
#include "log.h"
#include "nlohmann/json.hpp"

namespace api_manage {

namespace {

const std::size_t kMaxBodyBytes = 16 * 1024;

// 严格解析非负整数；非法输入返回 false，避免 stoll 抛异常。
bool parse_id(const std::string& text, long long& out) {
    if (text.empty() || text.size() > 18) {
        return false;
    }
    long long value = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + (c - '0');
    }
    out = value;
    return true;
}

}  // namespace

void handle_rename(const httplib::Request& req, httplib::Response& res) {
    long long user_id = 0;
    if (!auth::resolve(req, user_id)) {
        api::json_error(res, 401, "unauthorized", "登录已过期，请重新登录");
        return;
    }

    long long file_id = 0;
    if (!parse_id(req.matches[1].str(), file_id)) {
        api::json_error(res, 404, "not_found", "文件不存在");
        return;
    }

    if (req.body.size() > kMaxBodyBytes) {
        api::json_error(res, 413, "payload_too_large", "请求内容过大");
        return;
    }
    const nlohmann::json body = nlohmann::json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.is_object() || !body.contains("name") ||
        !body["name"].is_string()) {
        api::json_error(res, 400, "invalid_request", "缺少名称");
        return;
    }
    const std::string name = body["name"].get<std::string>();
    if (!api_files::valid_entry_name(name)) {
        api::json_error(res, 400, "invalid_name", "名称不能包含 / \\ : * ? \" < > | 等字符");
        return;
    }

    std::string final_name;
    std::string err;
    if (!db::files::rename_file(user_id, file_id, name, final_name, err)) {
        if (err == "not_found") {
            // 不存在 / 越权 / 已在回收站统一 404，不泄露他人条目是否存在。
            api::json_error(res, 404, "not_found", "文件不存在");
            return;
        }
        log_error("重命名失败：" + err);
        api::json_error(res, 500, "internal_error", "服务器内部错误");
        return;
    }

    nlohmann::json result;
    result["id"] = file_id;
    result["name"] = final_name;  // 冲突时已按 R2 改名，客户端必须用这个名称
    api::json_ok(res, result);
}

void register_routes(httplib::Server& svr) {
    svr.Patch(R"(/api/files/([0-9]+))", handle_rename);
}

}  // namespace api_manage
