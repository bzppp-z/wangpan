#include "db/blobs.h"

#include "db/db.h"

namespace db {
namespace blobs {

bool find_by_id(long long blob_id, Info& out) {
    out = Info();
    sqlite3* h = handle();
    if (h == nullptr) {
        return false;
    }
    const char* sql = "SELECT id, size, sha256 FROM blobs WHERE id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(h, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int64(stmt, 1, blob_id);
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        found = true;
        out.id = sqlite3_column_int64(stmt, 0);
        out.size = sqlite3_column_int64(stmt, 1);
        out.sha256 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    }
    sqlite3_finalize(stmt);
    return found;
}

bool get_or_create(const std::string& sha256, long long size, long long& out_id, std::string& err) {
    err.clear();
    out_id = 0;
    sqlite3* h = handle();
    if (h == nullptr) {
        err = "数据库未打开";
        return false;
    }

    // 先按唯一索引查：绝大多数情况下内容已存在（秒传 / 重复上传）。
    {
        const char* sql = "SELECT id, size FROM blobs WHERE sha256 = ?";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(h, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            err = sqlite3_errmsg(h);
            return false;
        }
        sqlite3_bind_text(stmt, 1, sha256.c_str(), -1, SQLITE_TRANSIENT);
        const int rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            const long long id = sqlite3_column_int64(stmt, 0);
            const long long stored_size = sqlite3_column_int64(stmt, 1);
            sqlite3_finalize(stmt);
            if (stored_size != size) {
                // 同一个 SHA-256 不可能对应两种长度，出现即内容损坏。
                err = "同哈希内容的长度不一致";
                return false;
            }
            out_id = id;
            return true;
        }
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) {
            err = sqlite3_errmsg(h);
            return false;
        }
    }

    const char* insert_sql =
        "INSERT INTO blobs(sha256, size, refcount, created_at) VALUES(?, ?, 0, ?)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(h, insert_sql, -1, &stmt, nullptr) != SQLITE_OK) {
        err = sqlite3_errmsg(h);
        return false;
    }
    sqlite3_bind_text(stmt, 1, sha256.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, size);
    sqlite3_bind_int64(stmt, 3, now_seconds());
    const int rc = sqlite3_step(stmt);
    const int extended = sqlite3_extended_errcode(h);
    const std::string message = sqlite3_errmsg(h);
    if (rc == SQLITE_DONE) {
        out_id = sqlite3_last_insert_rowid(h);
        sqlite3_finalize(stmt);
        return true;
    }
    sqlite3_finalize(stmt);
    if (extended != SQLITE_CONSTRAINT_UNIQUE) {
        err = message;
        return false;
    }

    // 并发下别的请求先插入了同一 sha：回读那一行。
    const char* select_sql = "SELECT id, size FROM blobs WHERE sha256 = ?";
    if (sqlite3_prepare_v2(h, select_sql, -1, &stmt, nullptr) != SQLITE_OK) {
        err = sqlite3_errmsg(h);
        return false;
    }
    sqlite3_bind_text(stmt, 1, sha256.c_str(), -1, SQLITE_TRANSIENT);
    const int rc2 = sqlite3_step(stmt);
    if (rc2 == SQLITE_ROW) {
        const long long id = sqlite3_column_int64(stmt, 0);
        const long long stored_size = sqlite3_column_int64(stmt, 1);
        sqlite3_finalize(stmt);
        if (stored_size != size) {
            err = "同哈希内容的长度不一致";
            return false;
        }
        out_id = id;
        return true;
    }
    sqlite3_finalize(stmt);
    err = "写入内容记录失败";
    return false;
}

bool incref(long long blob_id, std::string& err) {
    err.clear();
    sqlite3* h = handle();
    if (h == nullptr) {
        err = "数据库未打开";
        return false;
    }
    const char* sql = "UPDATE blobs SET refcount = refcount + 1 WHERE id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(h, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        err = sqlite3_errmsg(h);
        return false;
    }
    sqlite3_bind_int64(stmt, 1, blob_id);
    const int rc = sqlite3_step(stmt);
    const std::string message = sqlite3_errmsg(h);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        err = message;
        return false;
    }
    if (sqlite3_changes(h) != 1) {
        err = "内容记录不存在";
        return false;
    }
    return true;
}

}  // namespace blobs
}  // namespace db
