#include "api/files.h"

#include <cstddef>
#include <string>

#include "api/auth.h"
#include "api/respond.h"
#include "db/files.h"
#include "db/users.h"
#include "log.h"
#include "nlohmann/json.hpp"

namespace api_files {

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

// 鉴权 + 目录归属校验；失败时已写好 401/404，返回 false。
bool require_folder(const httplib::Request& req, httplib::Response& res, long long& out_user_id,
                    long long& out_parent_id) {
    out_user_id = 0;
    out_parent_id = 0;
    if (!auth::resolve(req, out_user_id)) {
        api::json_error(res, 401, "unauthorized", "登录已过期，请重新登录");
        return false;
    }

    if (req.has_param("parent_id")) {
        if (!parse_id(req.get_param_value("parent_id"), out_parent_id)) {
            api::json_error(res, 400, "invalid_parent", "parent_id 必须是非负整数");
            return false;
        }
    }

    std::string folder_name;
    if (!db::files::find_folder(out_user_id, out_parent_id, folder_name)) {
        api::json_error(res, 404, "not_found", "目录不存在");
        return false;
    }
    return true;
}

}  // namespace

bool valid_entry_name(const std::string& name) {
    if (name.empty() || name.size() > 255) {
        return false;
    }
    if (name == "." || name == "..") {
        return false;
    }
    const std::string forbidden = "/\\:*?\"<>|";
    for (std::size_t i = 0; i < name.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        if (c < 0x20 || c == 0x7F) {
            return false;
        }
        if (forbidden.find(static_cast<char>(c)) != std::string::npos) {
            return false;
        }
    }
    return true;
}

void handle_list(const httplib::Request& req, httplib::Response& res) {
    long long user_id = 0;
    long long parent_id = 0;
    if (!require_folder(req, res, user_id, parent_id)) {
        return;
    }

    std::vector<db::files::Row> rows;
    std::string err;
    if (!db::files::list_children(user_id, parent_id, rows, err)) {
        log_error("读取目录失败：" + err);
        api::json_error(res, 500, "internal_error", "服务器内部错误");
        return;
    }

    nlohmann::json items = nlohmann::json::array();
    for (std::size_t i = 0; i < rows.size(); ++i) {
        nlohmann::json item;
        item["id"] = rows[i].id;
        item["name"] = rows[i].name;
        item["kind"] = rows[i].kind;
        item["size"] = rows[i].size;
        item["updated_at"] = rows[i].updated_at;
        items.push_back(item);
    }

    nlohmann::json body;
    body["parent_id"] = parent_id;
    body["items"] = items;
    body["used_bytes"] = db::used_bytes(user_id);
    body["quota_bytes"] = db::quota_bytes_of(user_id);
    api::json_ok(res, body);
}

void handle_create_folder(const httplib::Request& req, httplib::Response& res) {
    long long user_id = 0;
    if (!auth::resolve(req, user_id)) {
        api::json_error(res, 401, "unauthorized", "登录已过期，请重新登录");
        return;
    }
    if (req.body.size() > kMaxBodyBytes) {
        api::json_error(res, 413, "payload_too_large", "请求内容过大");
        return;
    }

    const nlohmann::json body = nlohmann::json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.is_object()) {
        api::json_error(res, 400, "invalid_request", "请求体不是合法 JSON");
        return;
    }
    if (!body.contains("name") || !body["name"].is_string()) {
        api::json_error(res, 400, "invalid_request", "缺少名称");
        return;
    }
    const std::string name = body["name"].get<std::string>();
    if (!valid_entry_name(name)) {
        api::json_error(res, 400, "invalid_name", "名称不能包含 / \\ : * ? \" < > | 等字符");
        return;
    }

    long long parent_id = 0;
    if (body.contains("parent_id")) {
        if (!body["parent_id"].is_number_integer()) {
            api::json_error(res, 400, "invalid_parent", "parent_id 必须是整数");
            return;
        }
        const long long raw = body["parent_id"].get<long long>();
        if (raw < 0) {
            api::json_error(res, 400, "invalid_parent", "parent_id 必须是非负整数");
            return;
        }
        parent_id = raw;
    }

    std::string folder_name;
    if (!db::files::find_folder(user_id, parent_id, folder_name)) {
        api::json_error(res, 404, "not_found", "目录不存在");
        return;
    }

    long long folder_id = 0;
    std::string final_name;
    std::string err;
    if (!db::files::create_folder(user_id, parent_id, name, folder_id, final_name, err)) {
        log_error("新建文件夹失败：" + err);
        api::json_error(res, 500, "internal_error", "服务器内部错误");
        return;
    }

    nlohmann::json created;
    created["id"] = folder_id;
    created["name"] = final_name;  // 重名时已自动追加 (1)(2)，客户端必须用这个名称
    api::json_ok(res, created, 201);
}

void register_routes(httplib::Server& svr) {
    svr.Get("/api/files", handle_list);
    svr.Post("/api/folders", handle_create_folder);
}

}  // namespace api_files
