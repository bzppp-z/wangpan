#include "db/files.h"

#include <cstddef>

#include "db/db.h"

namespace db {
namespace files {

namespace {

// 文件名上限 255 字节（与 PRD R2 一致）。
const std::size_t kMaxNameBytes = 255;

// 按 UTF-8 字符边界截断，避免切出半个多字节字符。
std::string truncate_utf8(const std::string& text, std::size_t max_bytes) {
    if (text.size() <= max_bytes) {
        return text;
    }
    std::size_t cut = max_bytes;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return text.substr(0, cut);
}

// 第 0 个候选是原名，之后依次是 "name (1)"、"name (2)"…
std::string make_candidate(const std::string& desired, int index) {
    if (index == 0) {
        return truncate_utf8(desired, kMaxNameBytes);
    }
    const std::string suffix = " (" + std::to_string(index) + ")";
    if (suffix.size() >= kMaxNameBytes) {
        return suffix.substr(0, kMaxNameBytes);
    }
    return truncate_utf8(desired, kMaxNameBytes - suffix.size()) + suffix;
}

bool name_taken(long long owner_id, long long parent_id, const std::string& name) {
    sqlite3* h = handle();
    if (h == nullptr) {
        return false;
    }
    const char* sql =
        "SELECT 1 FROM files WHERE owner_id = ? AND parent_id = ? AND name = ?"
        " AND deleted_at IS NULL LIMIT 1";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(h, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int64(stmt, 1, owner_id);
    sqlite3_bind_int64(stmt, 2, parent_id);
    sqlite3_bind_text(stmt, 3, name.c_str(), -1, SQLITE_TRANSIENT);
    const bool taken = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return taken;
}

}  // namespace

bool list_children(long long owner_id, long long parent_id, std::vector<Row>& out,
                   std::string& err) {
    err.clear();
    out.clear();
    sqlite3* h = handle();
    if (h == nullptr) {
        err = "数据库未打开";
        return false;
    }

    // 文件夹在前，同类按名称字节序升序。
    const char* sql =
        "SELECT id, parent_id, name, kind, size, updated_at FROM files"
        " WHERE owner_id = ? AND parent_id = ? AND deleted_at IS NULL"
        " ORDER BY CASE kind WHEN 'folder' THEN 0 ELSE 1 END, name";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(h, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        err = sqlite3_errmsg(h);
        return false;
    }
    sqlite3_bind_int64(stmt, 1, owner_id);
    sqlite3_bind_int64(stmt, 2, parent_id);

    int rc = SQLITE_ROW;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        Row row;
        row.id = sqlite3_column_int64(stmt, 0);
        row.parent_id = sqlite3_column_int64(stmt, 1);
        row.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        row.kind = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        row.size = sqlite3_column_int64(stmt, 4);
        row.updated_at = sqlite3_column_int64(stmt, 5);
        out.push_back(row);
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        err = sqlite3_errmsg(h);
        return false;
    }
    return true;
}

std::string unique_name(long long owner_id, long long parent_id, const std::string& desired_name) {
    for (int i = 0; i < 100; ++i) {
        const std::string candidate = make_candidate(desired_name, i);
        if (!name_taken(owner_id, parent_id, candidate)) {
            return candidate;
        }
    }
    return make_candidate(desired_name, 100);
}

bool find_folder(long long owner_id, long long folder_id, std::string& out_name) {
    out_name.clear();
    if (folder_id == 0) {
        out_name = "我的网盘";
        return true;
    }
    sqlite3* h = handle();
    if (h == nullptr) {
        return false;
    }

    const char* sql =
        "SELECT name FROM files WHERE id = ? AND owner_id = ? AND kind = 'folder'"
        " AND deleted_at IS NULL";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(h, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int64(stmt, 1, folder_id);
    sqlite3_bind_int64(stmt, 2, owner_id);
    const bool found = (sqlite3_step(stmt) == SQLITE_ROW);
    if (found) {
        out_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    }
    sqlite3_finalize(stmt);
    return found;
}

bool create_folder(long long owner_id, long long parent_id, const std::string& name, long long& out_id,
                   std::string& out_name, std::string& err) {
    err.clear();
    out_id = 0;
    out_name.clear();
    sqlite3* h = handle();
    if (h == nullptr) {
        err = "数据库未打开";
        return false;
    }

    const long long now = now_seconds();
    const char* sql =
        "INSERT INTO files(owner_id, parent_id, name, kind, size, blob_id, deleted_at, created_at,"
        " updated_at) VALUES(?, ?, ?, 'folder', 0, NULL, NULL, ?, ?)";

    // 直接插入并依赖 idx_files_active_name 唯一索引，冲突就换下一个候选名。
    // 不做"先查后插"，否则并发下两个请求会同时通过检查。
    for (int i = 0; i < 100; ++i) {
        const std::string candidate = make_candidate(name, i);
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(h, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            err = sqlite3_errmsg(h);
            return false;
        }
        sqlite3_bind_int64(stmt, 1, owner_id);
        sqlite3_bind_int64(stmt, 2, parent_id);
        sqlite3_bind_text(stmt, 3, candidate.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 4, now);
        sqlite3_bind_int64(stmt, 5, now);

        const int rc = sqlite3_step(stmt);
        const int extended = sqlite3_extended_errcode(h);
        const std::string message = sqlite3_errmsg(h);
        if (rc == SQLITE_DONE) {
            out_id = sqlite3_last_insert_rowid(h);
            out_name = candidate;
            sqlite3_finalize(stmt);
            return true;
        }
        sqlite3_finalize(stmt);

        if (extended == SQLITE_CONSTRAINT_UNIQUE || extended == SQLITE_CONSTRAINT_PRIMARYKEY) {
            continue;  // 名称被占用，试下一个候选
        }
        err = message;
        return false;
    }

    err = "同名项过多，无法生成新名称";
    return false;
}

bool find_owned_by_sha(long long owner_id, const std::string& sha256, long long& out_file_id,
                       std::string& err) {
    // 桩实现：本提交只用于让 test_instant 可编译并「先红」。
    (void)owner_id;
    (void)sha256;
    out_file_id = 0;
    err.clear();
    return false;
}

bool create_file_entry(long long owner_id, long long parent_id, const std::string& desired_name,
                       long long blob_id, long long size, long long& out_id,
                       std::string& out_name, std::string& err) {
    // 桩实现：本提交只用于让 test_instant 可编译并「先红」。
    (void)owner_id;
    (void)parent_id;
    (void)desired_name;
    (void)blob_id;
    (void)size;
    out_id = 0;
    out_name.clear();
    err = "尚未实现";
    return false;
}

}  // namespace files
}  // namespace db
