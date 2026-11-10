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

// 已接收分片的上传会话上下文。
struct SessionCtx {
    std::string id;
    long long user_id = 0;
    long long parent_id = 0;
    long long size = 0;
    long long chunk_size = 0;
    long long chunk_count = 0;
    long long created_at = 0;
    std::string name;
    std::string sha256;
};

// 按 id + 归属读取会话；别人的会话与不存在一律视为不存在。
bool load_session(const std::string& session_id, long long user_id, SessionCtx& out) {
    sqlite3* h = db::handle();
    if (h == nullptr) {
        return false;
    }
    const char* sql =
        "SELECT owner_id, parent_id, name, size, sha256, chunk_size, chunk_count, created_at"
        " FROM upload_sessions WHERE id = ? AND owner_id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(h, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, user_id);
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        found = true;
        out.id = session_id;
        out.user_id = sqlite3_column_int64(stmt, 0);
        out.parent_id = sqlite3_column_int64(stmt, 1);
        out.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        out.size = sqlite3_column_int64(stmt, 3);
        out.sha256 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        out.chunk_size = sqlite3_column_int64(stmt, 5);
        out.chunk_count = sqlite3_column_int64(stmt, 6);
        out.created_at = sqlite3_column_int64(stmt, 7);
    }
    sqlite3_finalize(stmt);
    return found;
}

// 删除会话记录与临时目录（失败只记日志，不影响响应）。
void drop_session(const Config& cfg, const std::string& session_id) {
    std::string err;
    if (!delete_session_row(session_id, err)) {
        log_warn("删除上传会话失败：" + err);
    }
    if (!storage::remove_tree(session_dir_of(cfg, session_id))) {
        log_warn("删除上传临时目录失败：" + session_id);
    }
}

bool session_expired(const SessionCtx& s) {
    return s.created_at + kSessionTtlSeconds <= db::now_seconds();
}

// 鉴权 + 会话归属 + 有效期；失败时已写好 401/404。
bool require_session(const httplib::Request& req, httplib::Response& res, const Config& cfg,
                     SessionCtx& out) {
    long long user_id = 0;
    if (!auth::resolve(req, user_id)) {
        api::json_error(res, 401, "unauthorized", "登录已过期，请重新登录");
        return false;
    }
    const std::string session_id = req.matches[1];
    if (!load_session(session_id, user_id, out)) {
        api::json_error(res, 404, "session_not_found", "上传会话不存在");
        return false;
    }
    if (session_expired(out)) {
        drop_session(cfg, out.id);
        api::json_error(res, 404, "session_not_found", "上传会话已过期");
        return false;
    }
    return true;
}

// 解析纯数字下标；位数受限，避免 stoll 溢出。
bool parse_index(const std::string& text, long long& out) {
    if (text.empty() || text.size() > 9) {
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

void handle_put_chunk(const httplib::Request& req, httplib::Response& res, const Config& cfg) {
    SessionCtx session;
    if (!require_session(req, res, cfg, session)) {
        return;
    }

    long long index = 0;
    if (req.matches.size() < 3 || !parse_index(req.matches[2], index)) {
        api::json_error(res, 400, "invalid_chunk", "分片序号非法");
        return;
    }
    if (index >= session.chunk_count) {
        api::json_error(res, 400, "invalid_chunk", "分片序号超出范围");
        return;
    }
    const std::int64_t expected =
        static_cast<std::int64_t>(index) + 1 == session.chunk_count
            ? session.size - static_cast<std::int64_t>(index) * session.chunk_size
            : session.chunk_size;
    if (static_cast<std::int64_t>(req.body.size()) != expected) {
        api::json_error(res, 400, "chunk_size_mismatch", "分片长度与服务端约定的不一致");
        return;
    }

    std::string err;
    if (!storage::write_chunk(session_dir_of(cfg, session.id), static_cast<int>(index), req.body,
                              expected, err)) {
        log_error("写入分片失败：" + err);
        api::json_error(res, 500, "internal_error", "服务器内部错误");
        return;
    }
    res.status = 204;
}

void handle_status(const httplib::Request& req, httplib::Response& res, const Config& cfg) {
    SessionCtx session;
    if (!require_session(req, res, cfg, session)) {
        return;
    }
    std::vector<int> received;
    std::string err;
    if (!storage::scan_received(session_dir_of(cfg, session.id),
                                static_cast<int>(session.chunk_count), session.chunk_size,
                                session.size, received, err)) {
        log_error("扫描分片失败：" + err);
        api::json_error(res, 500, "internal_error", "服务器内部错误");
        return;
    }
    nlohmann::json out;
    out["session_id"] = session.id;
    out["size"] = session.size;
    out["chunk_size"] = session.chunk_size;
    out["chunk_count"] = session.chunk_count;
    out["received"] = received;
    out["expires_at"] = session.created_at + kSessionTtlSeconds;
    api::json_ok(res, out);
}

bool blob_exists(const std::string& path) {
    struct stat st;
    return !path.empty() && ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

void handle_complete(const httplib::Request& req, httplib::Response& res, const Config& cfg) {
    SessionCtx session;
    if (!require_session(req, res, cfg, session)) {
        return;
    }
    const std::string dir = session_dir_of(cfg, session.id);

    std::vector<int> received;
    std::string err;
    if (!storage::scan_received(dir, static_cast<int>(session.chunk_count), session.chunk_size,
                                session.size, received, err)) {
        log_error("扫描分片失败：" + err);
        api::json_error(res, 500, "internal_error", "服务器内部错误");
        return;
    }
    if (static_cast<long long>(received.size()) != session.chunk_count) {
        std::string missing = "?";
        std::size_t cursor = 0;
        for (int i = 0; i < session.chunk_count; ++i) {
            if (cursor < received.size() && received[cursor] == i) {
                ++cursor;
                continue;
            }
            missing = std::to_string(i);
            break;
        }
        api::json_error(res, 400, "invalid_chunk", "缺少第 " + missing + " 片");
        return;
    }

    // 合并前再查一次配额：上传期间账户可能又存了别的文件。
    const long long used = db::used_bytes(session.user_id);
    const long long quota = db::quota_bytes_of(session.user_id);
    if (quota > 0 && used + session.size > quota) {
        api::json_error(res, 507, "quota_exceeded", "空间不足，请先清理文件");
        return;
    }

    const std::string merged = dir + "/merged.tmp";
    std::string sha;
    if (!storage::merge_and_hash(dir, static_cast<int>(session.chunk_count), session.chunk_size,
                                 session.size, merged, sha, err)) {
        log_error("合并分片失败：" + err);
        api::json_error(res, 500, "internal_error", "服务器内部错误");
        return;
    }
    // 哈希由服务端计算，客户端的 sha 只用于比对；不符一律丢弃会话。
    if (sha != session.sha256) {
        log_warn("上传内容校验失败：" + session.id);
        drop_session(cfg, session.id);
        api::json_error(res, 400, "checksum_mismatch", "文件内容校验失败，请重新上传");
        return;
    }
    struct stat merged_st;
    if (::stat(merged.c_str(), &merged_st) != 0 ||
        static_cast<long long>(merged_st.st_size) != session.size) {
        drop_session(cfg, session.id);
        api::json_error(res, 400, "size_mismatch", "文件大小校验失败，请重新上传");
        return;
    }

    const bool dedup = blob_exists(storage::blob_path(cfg.data_dir, sha));
    long long blob_id = 0;
    if (!db::blobs::get_or_create(sha, session.size, blob_id, err)) {
        log_error("创建内容记录失败：" + err);
        api::json_error(res, 500, "internal_error", "服务器内部错误");
        return;
    }
    // 先把内容原子落位，再建数据库条目：崩溃时最多留下无引用的内容文件，
    // 不会出现"条目指向不存在内容"的坏状态。
    if (!storage::commit_blob(merged, cfg.data_dir, sha, err)) {
        log_error("内容落盘失败：" + err);
        api::json_error(res, 500, "internal_error", "服务器内部错误");
        return;
    }
    if (!db::blobs::incref(blob_id, err)) {
        log_error("增加引用计数失败：" + err);
        api::json_error(res, 500, "internal_error", "服务器内部错误");
        return;
    }
    long long file_id = 0;
    std::string final_name;
    if (!db::files::create_file_entry(session.user_id, session.parent_id, session.name, blob_id,
                                      session.size, file_id, final_name, err)) {
        log_error("创建文件条目失败：" + err);
        api::json_error(res, 500, "internal_error", "服务器内部错误");
        return;
    }

    std::string cleanup_err;
    if (!delete_session_row(session.id, cleanup_err)) {
        log_warn("删除上传会话记录失败：" + cleanup_err);
    }
    if (!storage::remove_tree(dir)) {
        log_warn("删除上传临时目录失败：" + session.id);
    }

    nlohmann::json out;
    out["file_id"] = file_id;
    out["name"] = final_name;
    out["sha256"] = sha;
    out["size"] = session.size;
    out["dedup"] = dedup;
    api::json_ok(res, out, 201);
}

void handle_cancel(const httplib::Request& req, httplib::Response& res, const Config& cfg) {
    long long user_id = 0;
    if (!auth::resolve(req, user_id)) {
        api::json_error(res, 401, "unauthorized", "登录已过期，请重新登录");
        return;
    }
    const std::string session_id = req.matches[1];
    SessionCtx session;
    if (!load_session(session_id, user_id, session)) {
        api::json_error(res, 404, "session_not_found", "上传会话不存在");
        return;
    }
    drop_session(cfg, session_id);
    res.status = 204;
}

}  // namespace

void register_routes(httplib::Server& svr, const Config& cfg) {
    svr.Post("/api/upload/init",
             [&cfg](const httplib::Request& req, httplib::Response& res) {
                 handle_init(req, res, cfg);
             });
    svr.Put(R"(/api/upload/([0-9a-f]{32})/chunks/([0-9]+))",
            [&cfg](const httplib::Request& req, httplib::Response& res) {
                handle_put_chunk(req, res, cfg);
            });
    svr.Get(R"(/api/upload/([0-9a-f]{32}))",
            [&cfg](const httplib::Request& req, httplib::Response& res) {
                handle_status(req, res, cfg);
            });
    svr.Post(R"(/api/upload/([0-9a-f]{32})/complete)",
             [&cfg](const httplib::Request& req, httplib::Response& res) {
                 handle_complete(req, res, cfg);
             });
    svr.Delete(R"(/api/upload/([0-9a-f]{32}))",
               [&cfg](const httplib::Request& req, httplib::Response& res) {
                   handle_cancel(req, res, cfg);
               });
}

}  // namespace api_upload
