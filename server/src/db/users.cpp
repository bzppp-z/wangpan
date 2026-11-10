#include "db/users.h"

#include "db/db.h"

namespace db {

bool create_user(const std::string& username, const std::string& salt_hex,
                 const std::string& hash_hex, long long quota_bytes, long long& out_id,
                 std::string& err) {
    err.clear();
    out_id = 0;
    sqlite3* h = handle();
    if (h == nullptr) {
        err = "数据库未打开";
        return false;
    }

    const char* sql =
        "INSERT INTO users(username, pass_salt, pass_hash, quota_bytes, created_at)"
        " VALUES(?, ?, ?, ?, ?)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(h, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        err = sqlite3_errmsg(h);
        return false;
    }
    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, salt_hex.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, hash_hex.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, quota_bytes);
    sqlite3_bind_int64(stmt, 5, now_seconds());

    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        err = (rc == SQLITE_CONSTRAINT) ? "用户名已存在" : sqlite3_errmsg(h);
        return false;
    }

    out_id = sqlite3_last_insert_rowid(h);
    return true;
}

bool find_user_by_name(const std::string& username, long long& out_id, std::string& out_salt,
                       std::string& out_hash, long long& out_quota, std::string& err) {
    err.clear();
    out_id = 0;
    out_salt.clear();
    out_hash.clear();
    out_quota = 0;
    sqlite3* h = handle();
    if (h == nullptr) {
        err = "数据库未打开";
        return false;
    }

    const char* sql = "SELECT id, pass_salt, pass_hash, quota_bytes FROM users WHERE username = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(h, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        err = sqlite3_errmsg(h);
        return false;
    }
    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);

    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        out_id = sqlite3_column_int64(stmt, 0);
        out_salt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        out_hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        out_quota = sqlite3_column_int64(stmt, 3);
        sqlite3_finalize(stmt);
        return true;
    }

    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        err = sqlite3_errmsg(h);
        return false;
    }
    return false;  // 用户不存在，err 保持为空
}

long long used_bytes(long long user_id) {
    sqlite3* h = handle();
    if (h == nullptr) {
        return 0;
    }

    // 含回收站内文件，符合 PRD R9。
    const char* sql =
        "SELECT COALESCE(SUM(size), 0) FROM files WHERE owner_id = ? AND kind = 'file'";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(h, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_int64(stmt, 1, user_id);
    long long total = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        total = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return total;
}

}  // namespace db
