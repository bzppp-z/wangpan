#include "api/upload.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>

#include "api/auth.h"
#include "api/files.h"
#include "api/respond.h"
#include "db/blobs.h"
#include "db/db.h"
#include "db/files.h"
#include "db/users.h"
#include "log.h"
#include "nlohmann/json.hpp"
#include "storage/blob.h"
#include "util/crypto.h"

namespace api_upload {

namespace {

const std::size_t kMaxBodyBytes = 16 * 1024;
const long long kMaxFileBytes = 10737418240LL;       // 10 GiB
const long long kSessionTtlSeconds = 7LL * 24 * 3600;  // PRD R4

// 只接受小写十六进制，避免同一内容出现两种字符串形式。
bool is_sha256_hex(const std::string& text) {
    if (text.size() != 64) {
        return false;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

long long chunk_count_for(long long size, long long chunk_size) {
    if (size <= 0 || chunk_size <= 0) {
        return 0;
    }
    return (size + chunk_size - 1) / chunk_size;
}

std::string session_dir_of(const Config& cfg, const std::string& session_id) {
    return cfg.data_dir + "/tmp/" + session_id;
}

// 建单层目录；已存在且是目录也算成功。
bool ensure_dir(const std::string& path) {
    if (::mkdir(path.c_str(), 0755) == 0) {
        return true;
    }
    if (errno != EEXIST) {
        return false;
    }
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

struct SessionInfo {
    std::string id;
    long long size = 0;
    long long chunk_size = 0;
    long long chunk_count = 0;
    long long created_at = 0;
};

// 查同参数、未过期的上传会话；有多条时取最新一条。
bool find_resumable(long long user_id, long long parent_id, const std::string& name, long long size,
                    const std::string& sha256, long long not_before, SessionInfo& out,
                    std::string& err) {
    err.clear();
    sqlite3* h = db::handle();
    if (h == nullptr) {
        err = "数据库未打开";
        return false;
    }
    const char* sql =
        "SELECT id, size, chunk_size, chunk_count, created_at FROM upload_sessions"
        " WHERE owner_id = ? AND parent_id = ? AND name = ? AND size = ? AND sha256 = ?"
        " AND created_at >= ? ORDER BY created_at DESC LIMIT 1";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(h, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        err = sqlite3_errmsg(h);
        return false;
    }
    sqlite3_bind_int64(stmt, 1, user_id);
    sqlite3_bind_int64(stmt, 2, parent_id);
    sqlite3_bind_text(stmt, 3, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, size);
    sqlite3_bind_text(stmt, 5, sha256.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 6, not_before);

    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        found = true;
        out.id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        out.size = sqlite3_column_int64(stmt, 1);
        out.chunk_size = sqlite3_column_int64(stmt, 2);
        out.chunk_count = sqlite3_column_int64(stmt, 3);
        out.created_at = sqlite3_column_int64(stmt, 4);
    }
    sqlite3_finalize(stmt);
    return found;
}

bool insert_session(const std::string& session_id, long long user_id, long long parent_id,
                    const std::string& name, long long size, const std::string& sha256,
                    long long chunk_size, long long chunk_count, long long created_at,
                    std::string& err) {
    err.clear();
    sqlite3* h = db::handle();
    if (h == nullptr) {
        err = "数据库未打开";
        return false;
    }
    const char* sql =
        "INSERT INTO upload_sessions(id, owner_id, parent_id, name, size, sha256, chunk_size,"
        " chunk_count, blob_id, created_at) VALUES(?, ?, ?, ?, ?, ?, ?, ?, NULL, ?)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(h, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        err = sqlite3_errmsg(h);
        return false;
    }
    sqlite3_bind_text(stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, user_id);
    sqlite3_bind_int64(stmt, 3, parent_id);
    sqlite3_bind_text(stmt, 4, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 5, size);
    sqlite3_bind_text(stmt, 6, sha256.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 7, chunk_size);
    sqlite3_bind_int64(stmt, 8, chunk_count);
    sqlite3_bind_int64(stmt, 9, created_at);
    const int rc = sqlite3_step(stmt);
    const std::string message = sqlite3_errmsg(h);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        err = message;
        return false;
    }
    return true;
}

// 删除一条会话记录（不删目录）。
bool delete_session_row(const std::string& session_id, std::string& err) {
    err.clear();
    sqlite3* h = db::handle();
    if (h == nullptr) {
        err = "数据库未打开";
        return false;
    }
    const char* sql = "DELETE FROM upload_sessions WHERE id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(h, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        err = sqlite3_errmsg(h);
        return false;
    }
    sqlite3_bind_text(stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(stmt);
    const std::string message = sqlite3_errmsg(h);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        err = message;
        return false;
    }
    return true;
}

// 清理该账户下同 sha 的过期会话：先删目录再删记录，失败只记日志。
void purge_expired(const Config& cfg, long long user_id, const std::string& sha256,
                   long long before) {
    sqlite3* h = db::handle();
    if (h == nullptr) {
        return;
    }
    std::vector<std::string> ids;
    {
        const char* sql =
            "SELECT id FROM upload_sessions WHERE owner_id = ? AND sha256 = ? AND created_at < ?";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(h, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return;
        }
        sqlite3_bind_int64(stmt, 1, user_id);
        sqlite3_bind_text(stmt, 2, sha256.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 3, before);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            ids.push_back(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
        }
        sqlite3_finalize(stmt);
    }
    for (std::size_t i = 0; i < ids.size(); ++i) {
        std::string err;
        if (!storage::remove_tree(session_dir_of(cfg, ids[i]))) {
            log_warn("清理过期上传目录失败：" + ids[i]);
        }
        if (!delete_session_row(ids[i], err)) {
            log_warn("清理过期上传记录失败：" + err);
        }
    }
}

// 从 JSON 读取非负整数；类型不对返回 false。
bool read_non_negative(const nlohmann::json& value, long long& out) {
    if (!value.is_number_integer()) {
        return false;
    }
    if (value.is_number_unsigned()) {
        const unsigned long long raw = value.get<unsigned long long>();
        if (raw > static_cast<unsigned long long>(9223372036854775807LL)) {
            return false;
        }
        out = static_cast<long long>(raw);
        return true;
    }
    const long long raw = value.get<long long>();
    if (raw < 0) {
        return false;
    }
    out = raw;
    return true;
}

void handle_init(const httplib::Request& req, httplib::Response& res, const Config& cfg) {
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
        api::json_error(res, 400, "invalid_name", "缺少文件名");
        return;
    }
    const std::string name = body["name"].get<std::string>();
    if (!api_files::valid_entry_name(name)) {
        api::json_error(res, 400, "invalid_name", "名称不能包含 / \\ : * ? \" < > | 等字符");
        return;
    }

    long long parent_id = 0;
    if (body.contains("parent_id")) {
        if (!read_non_negative(body["parent_id"], parent_id)) {
            api::json_error(res, 400, "invalid_parent", "parent_id 必须是非负整数");
            return;
        }
    }

    long long size = 0;
    if (!body.contains("size") || !read_non_negative(body["size"], size)) {
        api::json_error(res, 400, "invalid_size", "文件大小必须是非负整数");
        return;
    }
    if (size > kMaxFileBytes) {
        api::json_error(res, 400, "file_too_large", "单个文件不能超过 10 GB");
        return;
    }

    if (!body.contains("sha256") || !body["sha256"].is_string()) {
        api::json_error(res, 400, "sha256_invalid", "缺少 sha256");
        return;
    }
    const std::string sha256 = body["sha256"].get<std::string>();
    if (!is_sha256_hex(sha256)) {
        api::json_error(res, 400, "sha256_invalid", "sha256 必须是 64 位小写十六进制");
        return;
    }

    std::string folder_name;
    if (!db::files::find_folder(user_id, parent_id, folder_name)) {
        api::json_error(res, 404, "not_found", "目录不存在");
        return;
    }

    const long long used = db::used_bytes(user_id);
    const long long quota = db::quota_bytes_of(user_id);
    if (quota > 0 && used + size > quota) {
        api::json_error(res, 507, "quota_exceeded", "空间不足，请先清理文件");
        return;
    }

    std::string err;

    // 秒传：只认本账户自己已有的内容，避免跨账户白拿别人的文件。
    long long owned_file_id = 0;
    if (db::files::find_owned_by_sha(user_id, sha256, owned_file_id, err)) {
        long long blob_id = 0;
        if (!db::blobs::get_or_create(sha256, size, blob_id, err)) {
            log_error("秒传取内容记录失败：" + err);
            api::json_error(res, 500, "internal_error", "服务器内部错误");
            return;
        }
        if (!db::blobs::incref(blob_id, err)) {
            log_error("秒传增加引用计数失败：" + err);
            api::json_error(res, 500, "internal_error", "服务器内部错误");
            return;
        }
        long long new_file_id = 0;
        std::string final_name;
        if (!db::files::create_file_entry(user_id, parent_id, name, blob_id, size, new_file_id,
                                          final_name, err)) {
            log_error("秒传创建文件条目失败：" + err);
            api::json_error(res, 500, "internal_error", "服务器内部错误");
            return;
        }
        nlohmann::json out;
        out["instant"] = true;
        out["file_id"] = new_file_id;
        out["name"] = final_name;
        out["sha256"] = sha256;
        out["size"] = size;
        api::json_ok(res, out, 201);
        return;
    }

    const long long now = db::now_seconds();

    // 续传：同 parent + 同名 + 同大小 + 同 sha，7 天内且目录还在。
    SessionInfo session;
    if (find_resumable(user_id, parent_id, name, size, sha256, now - kSessionTtlSeconds, session,
                       err)) {
        std::vector<int> received;
        if (storage::scan_received(session_dir_of(cfg, session.id),
                                   static_cast<int>(session.chunk_count), session.chunk_size, size,
                                   received, err)) {
            nlohmann::json out;
            out["instant"] = false;
            out["session_id"] = session.id;
            out["chunk_size"] = session.chunk_size;
            out["chunk_count"] = session.chunk_count;
            out["received"] = received;
            out["expires_at"] = session.created_at + kSessionTtlSeconds;
            api::json_ok(res, out);
            return;
        }
        // 目录被清掉或分片不可读：丢掉这条记录，按全新会话处理。
        log_warn("上传会话目录不可用，重新开始：" + session.id);
        delete_session_row(session.id, err);
    }

    purge_expired(cfg, user_id, sha256, now - kSessionTtlSeconds);

    const std::string session_id = crypto::random_hex(16);
    if (session_id.size() != 32) {
        log_error("生成上传会话 id 失败");
        api::json_error(res, 500, "internal_error", "服务器内部错误");
        return;
    }
    const std::string tmp_root = cfg.data_dir + "/tmp";
    if (!ensure_dir(tmp_root) || !ensure_dir(session_dir_of(cfg, session_id))) {
        log_error("创建上传会话目录失败：" + session_dir_of(cfg, session_id));
        api::json_error(res, 500, "internal_error", "服务器内部错误");
        return;
    }

    const long long chunk_count = chunk_count_for(size, static_cast<long long>(cfg.chunk_size));
    if (!insert_session(session_id, user_id, parent_id, name, size, sha256,
                        static_cast<long long>(cfg.chunk_size), chunk_count, now, err)) {
        log_error("写入上传会话失败：" + err);
        storage::remove_tree(session_dir_of(cfg, session_id));
        api::json_error(res, 500, "internal_error", "服务器内部错误");
        return;
    }

    nlohmann::json out;
    out["instant"] = false;
    out["session_id"] = session_id;
    out["chunk_size"] = static_cast<long long>(cfg.chunk_size);
    out["chunk_count"] = chunk_count;
    out["received"] = std::vector<int>();
    out["expires_at"] = now + kSessionTtlSeconds;
    api::json_ok(res, out);
}

}  // namespace

void register_routes(httplib::Server& svr, const Config& cfg) {
    svr.Post("/api/upload/init",
             [&cfg](const httplib::Request& req, httplib::Response& res) {
                 handle_init(req, res, cfg);
             });
}

}  // namespace api_upload
