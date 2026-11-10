#include "api/trash.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "api/auth.h"
#include "api/respond.h"
#include "db/files.h"
#include "log.h"
#include "nlohmann/json.hpp"
#include "storage/blob.h"

namespace api_trash {

namespace {

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

void remove_blob_files(const Config& cfg, const std::vector<std::string>& shas) {
    for (std::size_t i = 0; i < shas.size(); ++i) {
        const std::string path = storage::blob_path(cfg.data_dir, shas[i]);
        if (std::remove(path.c_str()) != 0 && errno != ENOENT) {
            log_warn("删除内容文件失败：" + path + "（" + std::strerror(errno) + "）");
        }
    }
}

void handle_list(const httplib::Request& req, httplib::Response& res) {
    long long user_id = 0;
    if (!auth::resolve(req, user_id)) {
        api::json_error(res, 401, "unauthorized", "登录已过期，请重新登录");
        return;
    }

    std::vector<db::files::TrashRow> rows;
    std::string err;
    if (!db::files::list_trash(user_id, rows, err)) {
        log_error("读取回收站失败：" + err);
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
        item["deleted_at"] = rows[i].deleted_at;
        item["expires_at"] = rows[i].expires_at;
        item["parent_id"] = rows[i].parent_id;
        item["parent_name"] = rows[i].parent_name;
        items.push_back(item);
    }

    nlohmann::json body;
    body["items"] = items;
    api::json_ok(res, body);
}

void handle_restore(const httplib::Request& req, httplib::Response& res) {
    long long user_id = 0;
    if (!auth::resolve(req, user_id)) {
        api::json_error(res, 401, "unauthorized", "登录已过期，请重新登录");
        return;
    }
    long long file_id = 0;
    if (!parse_id(req.matches[1].str(), file_id)) {
        api::json_error(res, 404, "not_found", "回收站条目不存在");
        return;
    }

    std::string final_name;
    long long parent_id = 0;
    std::string err;
    if (!db::files::restore_subtree(user_id, file_id, final_name, parent_id, err)) {
        if (err == "not_found") {
            api::json_error(res, 404, "not_found", "回收站条目不存在");
            return;
        }
        log_error("恢复回收站条目失败：" + err);
        api::json_error(res, 500, "internal_error", "服务器内部错误");
        return;
    }

    nlohmann::json body;
    body["id"] = file_id;
    body["name"] = final_name;  // 原位置重名时已按 R2 改名
    body["parent_id"] = parent_id;
    api::json_ok(res, body);
}

void handle_purge(const httplib::Request& req, httplib::Response& res, const Config& cfg) {
    long long user_id = 0;
    if (!auth::resolve(req, user_id)) {
        api::json_error(res, 401, "unauthorized", "登录已过期，请重新登录");
        return;
    }
    long long file_id = 0;
    if (!parse_id(req.matches[1].str(), file_id)) {
        api::json_error(res, 404, "not_found", "回收站条目不存在");
        return;
    }

    std::vector<std::string> shas;
    std::string err;
    if (!db::files::purge_subtree(user_id, file_id, shas, err)) {
        if (err == "not_found") {
            api::json_error(res, 404, "not_found", "回收站条目不存在");
            return;
        }
        log_error("彻底删除失败：" + err);
        api::json_error(res, 500, "internal_error", "服务器内部错误");
        return;
    }
    remove_blob_files(cfg, shas);
    res.status = 204;
}

void handle_clear(const httplib::Request& req, httplib::Response& res, const Config& cfg) {
    long long user_id = 0;
    if (!auth::resolve(req, user_id)) {
        api::json_error(res, 401, "unauthorized", "登录已过期，请重新登录");
        return;
    }

    // 只取子树根逐个彻底删除；子孙随根一起清理，避免重复减引用。
    std::vector<db::files::TrashRow> rows;
    std::string err;
    if (!db::files::list_trash(user_id, rows, err)) {
        log_error("读取回收站失败：" + err);
        api::json_error(res, 500, "internal_error", "服务器内部错误");
        return;
    }

    std::vector<std::string> all_shas;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        std::vector<std::string> shas;
        std::string purge_err;
        if (!db::files::purge_subtree(user_id, rows[i].id, shas, purge_err)) {
            if (purge_err == "not_found") {
                continue;  // 并发下已被别的请求清理
            }
            log_error("清空回收站失败：" + purge_err);
            api::json_error(res, 500, "internal_error", "服务器内部错误");
            return;
        }
        all_shas.insert(all_shas.end(), shas.begin(), shas.end());
    }
    remove_blob_files(cfg, all_shas);
    res.status = 204;
}

void register_routes(httplib::Server& svr, const Config& cfg) {
    svr.Get("/api/trash", handle_list);
    svr.Post(R"(/api/trash/([0-9]+)/restore)", handle_restore);
    svr.Delete(R"(/api/trash/([0-9]+))",
               [&cfg](const httplib::Request& req, httplib::Response& res) {
                   handle_purge(req, res, cfg);
               });
    svr.Delete("/api/trash", [&cfg](const httplib::Request& req, httplib::Response& res) {
        handle_clear(req, res, cfg);
    });
}

}  // namespace api_trash
