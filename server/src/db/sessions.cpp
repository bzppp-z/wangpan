#include "db/sessions.h"

#include "db/db.h"

namespace db {

bool create_session(long long user_id, const std::string& token_hash, long long expires_at,
                    std::string& err) {
    err.clear();
    sqlite3* h = handle();
    if (h == nullptr) {
        err = "数据库未打开";
        return false;
    }

    const char* sql =
        "INSERT INTO sessions(token_hash, user_id, expires_at, created_at) VALUES(?, ?, ?, ?)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(h, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        err = sqlite3_errmsg(h);
        return false;
    }
    sqlite3_bind_text(stmt, 1, token_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, user_id);
    sqlite3_bind_int64(stmt, 3, expires_at);
    sqlite3_bind_int64(stmt, 4, now_seconds());

    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        err = sqlite3_errmsg(h);
        return false;
    }
    return true;
}

bool find_session(const std::string& token_hash, long long& out_user_id, long long& out_expires_at,
                  std::string& err) {
    err.clear();
    out_user_id = 0;
    out_expires_at = 0;
    sqlite3* h = handle();
    if (h == nullptr) {
        err = "数据库未打开";
        return false;
    }

    const char* sql = "SELECT user_id, expires_at FROM sessions WHERE token_hash = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(h, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        err = sqlite3_errmsg(h);
        return false;
    }
    sqlite3_bind_text(stmt, 1, token_hash.c_str(), -1, SQLITE_TRANSIENT);

    const int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) {
            err = sqlite3_errmsg(h);
        }
        return false;  // 会话不存在
    }

    const long long user_id = sqlite3_column_int64(stmt, 0);
    const long long expires_at = sqlite3_column_int64(stmt, 1);
    sqlite3_finalize(stmt);

    // 过期即视为无效；调用方无需再判断时间。
    if (expires_at <= now_seconds()) {
        return false;
    }
    out_user_id = user_id;
    out_expires_at = expires_at;
    return true;
}

bool delete_session(const std::string& token_hash, std::string& err) {
    err.clear();
    sqlite3* h = handle();
    if (h == nullptr) {
        err = "数据库未打开";
        return false;
    }

    const char* sql = "DELETE FROM sessions WHERE token_hash = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(h, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        err = sqlite3_errmsg(h);
        return false;
    }
    sqlite3_bind_text(stmt, 1, token_hash.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        err = sqlite3_errmsg(h);
        return false;
    }
    return true;
}

int delete_expired_sessions() {
    sqlite3* h = handle();
    if (h == nullptr) {
        return -1;
    }

    const char* sql = "DELETE FROM sessions WHERE expires_at <= ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(h, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_int64(stmt, 1, now_seconds());
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return -1;
    }
    return sqlite3_changes(h);
}

}  // namespace db
