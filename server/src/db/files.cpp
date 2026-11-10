#include "db/files.h"

#include <cstddef>

#include "db/blobs.h"
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

bool find_file(long long owner_id, long long file_id, Row& out) {
    out = Row();
    sqlite3* h = handle();
    if (h == nullptr) {
        return false;
    }

    const char* sql =
        "SELECT id, parent_id, name, kind, size, updated_at, blob_id FROM files"
        " WHERE id = ? AND owner_id = ? AND kind = 'file' AND deleted_at IS NULL";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(h, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int64(stmt, 1, file_id);
    sqlite3_bind_int64(stmt, 2, owner_id);
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        found = true;
        out.id = sqlite3_column_int64(stmt, 0);
        out.parent_id = sqlite3_column_int64(stmt, 1);
        out.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        out.kind = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        out.size = sqlite3_column_int64(stmt, 4);
        out.updated_at = sqlite3_column_int64(stmt, 5);
        out.blob_id = sqlite3_column_int64(stmt, 6);
    }
    sqlite3_finalize(stmt);
    return found;
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

bool rename_file(long long owner_id, long long file_id, const std::string& desired_name,
                 std::string& out_name, std::string& err) {
    err.clear();
    out_name.clear();
    sqlite3* h = handle();
    if (h == nullptr) {
        err = "数据库未打开";
        return false;
    }

    std::string current_name;
    {
        const char* sql =
            "SELECT name FROM files"
            " WHERE id = ? AND owner_id = ? AND deleted_at IS NULL";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(h, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            err = sqlite3_errmsg(h);
            return false;
        }
        sqlite3_bind_int64(stmt, 1, file_id);
        sqlite3_bind_int64(stmt, 2, owner_id);
        const int rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            current_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        }
        sqlite3_finalize(stmt);
        if (rc != SQLITE_ROW) {
            if (rc == SQLITE_DONE) {
                err = "not_found";
            } else {
                err = sqlite3_errmsg(h);
            }
            return false;
        }
    }

    // 名称未变化：既是无操作，也避免 unique_name 把自身算成冲突得到 "name (1)"。
    if (current_name == truncate_utf8(desired_name, kMaxNameBytes)) {
        out_name = current_name;
        return true;
    }

    const long long now = now_seconds();
    const char* sql = "UPDATE files SET name = ?, updated_at = ? WHERE id = ? AND owner_id = ?";
    // 与 create_folder 一致：直接 UPDATE 并依赖唯一索引，冲突就换下一个候选名。
    for (int i = 0; i < 100; ++i) {
        const std::string candidate = make_candidate(desired_name, i);
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(h, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            err = sqlite3_errmsg(h);
            return false;
        }
        sqlite3_bind_text(stmt, 1, candidate.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, now);
        sqlite3_bind_int64(stmt, 3, file_id);
        sqlite3_bind_int64(stmt, 4, owner_id);

        const int rc = sqlite3_step(stmt);
        const int extended = sqlite3_extended_errcode(h);
        const std::string message = sqlite3_errmsg(h);
        sqlite3_finalize(stmt);
        if (rc == SQLITE_DONE) {
            out_name = candidate;
            return true;
        }
        if (extended == SQLITE_CONSTRAINT_UNIQUE || extended == SQLITE_CONSTRAINT_PRIMARYKEY) {
            continue;  // 名称被占用，试下一个候选
        }
        err = message;
        return false;
    }

    err = "同名项过多，无法生成新名称";
    return false;
}

namespace {

// 回收站保留期：30 天（PRD R3）。
const long long kTrashRetentionSeconds = 30LL * 24 * 3600;

bool exec_ok(sqlite3* h, const char* sql, std::string& err) {
    char* message = nullptr;
    if (sqlite3_exec(h, sql, nullptr, nullptr, &message) != SQLITE_OK) {
        err = message != nullptr ? message : "SQL 执行失败";
        if (message != nullptr) {
            sqlite3_free(message);
        }
        return false;
    }
    return true;
}

void rollback(sqlite3* h) {
    sqlite3_exec(h, "ROLLBACK", nullptr, nullptr, nullptr);
}

// 校验 id 是当前账户回收站里的"子树根"：已删除，且父节点不存在或未删除。
// 命中时把名称与父目录写入输出参数。
bool find_trash_root(sqlite3* h, long long owner_id, long long file_id, std::string& out_name,
                     long long& out_parent_id) {
    const char* sql =
        "SELECT f.name, f.parent_id FROM files f LEFT JOIN files p ON p.id = f.parent_id"
        " WHERE f.id = ? AND f.owner_id = ? AND f.deleted_at IS NOT NULL"
        " AND (f.parent_id = 0 OR p.deleted_at IS NULL)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(h, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int64(stmt, 1, file_id);
    sqlite3_bind_int64(stmt, 2, owner_id);
    const int rc = sqlite3_step(stmt);
    bool found = false;
    if (rc == SQLITE_ROW) {
        found = true;
        out_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        out_parent_id = sqlite3_column_int64(stmt, 1);
    }
    sqlite3_finalize(stmt);
    return found;
}

// 把一条回收站记录恢复为活动状态，名称按 R2 去重；parent_id 由调用方决定。
bool activate_entry(sqlite3* h, long long owner_id, long long file_id, long long parent_id,
                    const std::string& desired_name, long long now, std::string& out_name,
                    std::string& err) {
    const char* sql =
        "UPDATE files SET deleted_at = NULL, name = ?, parent_id = ?, updated_at = ?"
        " WHERE id = ? AND owner_id = ? AND deleted_at IS NOT NULL";
    for (int i = 0; i < 100; ++i) {
        const std::string candidate = make_candidate(desired_name, i);
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(h, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            err = sqlite3_errmsg(h);
            return false;
        }
        sqlite3_bind_text(stmt, 1, candidate.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, parent_id);
        sqlite3_bind_int64(stmt, 3, now);
        sqlite3_bind_int64(stmt, 4, file_id);
        sqlite3_bind_int64(stmt, 5, owner_id);

        const int rc = sqlite3_step(stmt);
        const int extended = sqlite3_extended_errcode(h);
        const std::string message = sqlite3_errmsg(h);
        sqlite3_finalize(stmt);
        if (rc == SQLITE_DONE) {
            out_name = candidate;
            return true;
        }
        if (extended == SQLITE_CONSTRAINT_UNIQUE || extended == SQLITE_CONSTRAINT_PRIMARYKEY) {
            continue;  // 目标目录已有同名项，换下一个候选
        }
        err = message;
        return false;
    }
    err = "同名项过多，无法恢复";
    return false;
}

}  // namespace

bool soft_delete_subtree(long long owner_id, long long file_id, std::string& err) {
    err.clear();
    sqlite3* h = handle();
    if (h == nullptr) {
        err = "数据库未打开";
        return false;
    }
    if (!exec_ok(h, "BEGIN IMMEDIATE", err)) {
        return false;
    }

    {
        const char* sql = "SELECT 1 FROM files WHERE id = ? AND owner_id = ? AND deleted_at IS NULL";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(h, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            err = sqlite3_errmsg(h);
            rollback(h);
            return false;
        }
        sqlite3_bind_int64(stmt, 1, file_id);
        sqlite3_bind_int64(stmt, 2, owner_id);
        const int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_ROW) {
            rollback(h);
            if (rc == SQLITE_DONE) {
                err = "not_found";
            } else {
                err = sqlite3_errmsg(h);
            }
            return false;
        }
    }

    // 递归不过滤 deleted_at：把子树里先前单独删除的子孙也统一成同一个删除时间。
    const char* sql =
        "WITH RECURSIVE sub(id) AS ("
        "  SELECT id FROM files WHERE id = ? AND owner_id = ?"
        "  UNION ALL"
        "  SELECT f.id FROM files f JOIN sub s ON f.parent_id = s.id WHERE f.owner_id = ?"
        ") UPDATE files SET deleted_at = ? WHERE id IN (SELECT id FROM sub)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(h, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        err = sqlite3_errmsg(h);
        rollback(h);
        return false;
    }
    const long long now = now_seconds();
    sqlite3_bind_int64(stmt, 1, file_id);
    sqlite3_bind_int64(stmt, 2, owner_id);
    sqlite3_bind_int64(stmt, 3, owner_id);
    sqlite3_bind_int64(stmt, 4, now);
    const int rc = sqlite3_step(stmt);
    const std::string message = sqlite3_errmsg(h);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        err = message;
        rollback(h);
        return false;
    }
    if (!exec_ok(h, "COMMIT", err)) {
        rollback(h);
        return false;
    }
    return true;
}

bool list_trash(long long owner_id, std::vector<TrashRow>& out, std::string& err) {
    err.clear();
    out.clear();
    sqlite3* h = handle();
    if (h == nullptr) {
        err = "数据库未打开";
        return false;
    }

    // 父节点已删除的条目属于子树内部，不出现在列表里（父行缺失时按根处理）。
    const char* sql =
        "SELECT f.id, f.parent_id, f.name, f.kind, f.size, f.deleted_at,"
        " COALESCE(p.name, '') FROM files f LEFT JOIN files p ON p.id = f.parent_id"
        " WHERE f.owner_id = ? AND f.deleted_at IS NOT NULL"
        " AND (f.parent_id = 0 OR p.deleted_at IS NULL)"
        " ORDER BY f.deleted_at DESC, f.id DESC";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(h, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        err = sqlite3_errmsg(h);
        return false;
    }
    sqlite3_bind_int64(stmt, 1, owner_id);
    int rc = SQLITE_ROW;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        TrashRow row;
        row.id = sqlite3_column_int64(stmt, 0);
        row.parent_id = sqlite3_column_int64(stmt, 1);
        row.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        row.kind = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        row.size = sqlite3_column_int64(stmt, 4);
        row.deleted_at = sqlite3_column_int64(stmt, 5);
        row.expires_at = row.deleted_at + kTrashRetentionSeconds;
        row.parent_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        out.push_back(row);
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        err = sqlite3_errmsg(h);
        return false;
    }
    return true;
}

bool restore_subtree(long long owner_id, long long file_id, std::string& out_name,
                     long long& out_parent_id, std::string& err) {
    err.clear();
    out_name.clear();
    out_parent_id = 0;
    sqlite3* h = handle();
    if (h == nullptr) {
        err = "数据库未打开";
        return false;
    }
    if (!exec_ok(h, "BEGIN IMMEDIATE", err)) {
        return false;
    }

    std::string original_name;
    long long original_parent = 0;
    if (!find_trash_root(h, owner_id, file_id, original_name, original_parent)) {
        rollback(h);
        err = "not_found";
        return false;
    }

    // 原父目录已删除或不存在时恢复到根目录。
    long long target_parent = 0;
    if (original_parent != 0) {
        const char* sql =
            "SELECT 1 FROM files WHERE id = ? AND owner_id = ? AND kind = 'folder'"
            " AND deleted_at IS NULL";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(h, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            err = sqlite3_errmsg(h);
            rollback(h);
            return false;
        }
        sqlite3_bind_int64(stmt, 1, original_parent);
        sqlite3_bind_int64(stmt, 2, owner_id);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            target_parent = original_parent;
        }
        sqlite3_finalize(stmt);
    }

    // 按层级从浅到深恢复，保证父目录先于子项回到活动状态；子项重名时同样自动改名。
    struct Item {
        long long id = 0;
        long long parent_id = 0;
        std::string name;
    };
    std::vector<Item> items;
    {
        const char* sql =
            "WITH RECURSIVE sub(id, parent_id, name, depth) AS ("
            "  SELECT id, parent_id, name, 0 FROM files WHERE id = ? AND owner_id = ?"
            "  UNION ALL"
            "  SELECT f.id, f.parent_id, f.name, s.depth + 1 FROM files f"
            "    JOIN sub s ON f.parent_id = s.id WHERE f.owner_id = ?"
            ") SELECT id, parent_id, name FROM sub ORDER BY depth, id";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(h, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            err = sqlite3_errmsg(h);
            rollback(h);
            return false;
        }
        sqlite3_bind_int64(stmt, 1, file_id);
        sqlite3_bind_int64(stmt, 2, owner_id);
        sqlite3_bind_int64(stmt, 3, owner_id);
        int rc = SQLITE_ROW;
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            Item item;
            item.id = sqlite3_column_int64(stmt, 0);
            item.parent_id = sqlite3_column_int64(stmt, 1);
            item.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            items.push_back(item);
        }
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) {
            err = sqlite3_errmsg(h);
            rollback(h);
            return false;
        }
    }

    const long long now = now_seconds();
    for (std::size_t i = 0; i < items.size(); ++i) {
        const bool is_root = (items[i].id == file_id);
        const long long parent = is_root ? target_parent : items[i].parent_id;
        std::string final_name;
        if (!activate_entry(h, owner_id, items[i].id, parent, items[i].name, now, final_name,
                            err)) {
            rollback(h);
            return false;
        }
        if (is_root) {
            out_name = final_name;
            out_parent_id = parent;
        }
    }

    if (!exec_ok(h, "COMMIT", err)) {
        rollback(h);
        return false;
    }
    return true;
}

bool purge_subtree(long long owner_id, long long file_id, std::vector<std::string>& out_shas,
                   std::string& err) {
    err.clear();
    out_shas.clear();
    sqlite3* h = handle();
    if (h == nullptr) {
        err = "数据库未打开";
        return false;
    }
    if (!exec_ok(h, "BEGIN IMMEDIATE", err)) {
        return false;
    }

    std::string ignored_name;
    long long ignored_parent = 0;
    if (!find_trash_root(h, owner_id, file_id, ignored_name, ignored_parent)) {
        rollback(h);
        err = "not_found";
        return false;
    }

    // 先收集子树内所有 blob 引用（同一内容被多个条目引用时按条目数逐次减引用）。
    std::vector<long long> blob_ids;
    {
        const char* sql =
            "WITH RECURSIVE sub(id) AS ("
            "  SELECT id FROM files WHERE id = ? AND owner_id = ?"
            "  UNION ALL"
            "  SELECT f.id FROM files f JOIN sub s ON f.parent_id = s.id WHERE f.owner_id = ?"
            ") SELECT blob_id FROM files WHERE id IN (SELECT id FROM sub) AND blob_id IS NOT NULL";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(h, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            err = sqlite3_errmsg(h);
            rollback(h);
            return false;
        }
        sqlite3_bind_int64(stmt, 1, file_id);
        sqlite3_bind_int64(stmt, 2, owner_id);
        sqlite3_bind_int64(stmt, 3, owner_id);
        int rc = SQLITE_ROW;
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            blob_ids.push_back(sqlite3_column_int64(stmt, 0));
        }
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) {
            err = sqlite3_errmsg(h);
            rollback(h);
            return false;
        }
    }

    // 先删 files 行再减引用：files.blob_id 有外键指向 blobs，顺序反过来会触发
    // FOREIGN KEY constraint failed。两件事在同一事务里，失败一起回滚。
    {
        const char* sql =
            "WITH RECURSIVE sub(id) AS ("
            "  SELECT id FROM files WHERE id = ? AND owner_id = ?"
            "  UNION ALL"
            "  SELECT f.id FROM files f JOIN sub s ON f.parent_id = s.id WHERE f.owner_id = ?"
            ") DELETE FROM files WHERE id IN (SELECT id FROM sub) AND owner_id = ?";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(h, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            err = sqlite3_errmsg(h);
            rollback(h);
            return false;
        }
        sqlite3_bind_int64(stmt, 1, file_id);
        sqlite3_bind_int64(stmt, 2, owner_id);
        sqlite3_bind_int64(stmt, 3, owner_id);
        sqlite3_bind_int64(stmt, 4, owner_id);
        const int rc = sqlite3_step(stmt);
        const std::string message = sqlite3_errmsg(h);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) {
            err = message;
            rollback(h);
            return false;
        }
    }

    for (std::size_t i = 0; i < blob_ids.size(); ++i) {
        std::string sha;
        bool removed = false;
        if (!db::blobs::decref_and_collect(blob_ids[i], sha, removed, err)) {
            rollback(h);
            return false;
        }
        if (removed && !sha.empty()) {
            out_shas.push_back(sha);
        }
    }

    if (!exec_ok(h, "COMMIT", err)) {
        rollback(h);
        out_shas.clear();
        return false;
    }
    return true;
}

bool purge_expired(long long now, std::vector<std::string>& out_shas, std::string& err) {
    err.clear();
    out_shas.clear();
    sqlite3* h = handle();
    if (h == nullptr) {
        err = "数据库未打开";
        return false;
    }

    const long long cutoff = now - kTrashRetentionSeconds;
    struct Expired {
        long long id = 0;
        long long owner_id = 0;
    };
    std::vector<Expired> expired;
    {
        const char* sql =
            "SELECT f.id, f.owner_id FROM files f LEFT JOIN files p ON p.id = f.parent_id"
            " WHERE f.deleted_at IS NOT NULL AND f.deleted_at <= ?"
            " AND (f.parent_id = 0 OR p.deleted_at IS NULL)";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(h, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            err = sqlite3_errmsg(h);
            return false;
        }
        sqlite3_bind_int64(stmt, 1, cutoff);
        int rc = SQLITE_ROW;
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            Expired item;
            item.id = sqlite3_column_int64(stmt, 0);
            item.owner_id = sqlite3_column_int64(stmt, 1);
            expired.push_back(item);
        }
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) {
            err = sqlite3_errmsg(h);
            return false;
        }
    }

    for (std::size_t i = 0; i < expired.size(); ++i) {
        std::vector<std::string> shas;
        std::string purge_err;
        if (!purge_subtree(expired[i].owner_id, expired[i].id, shas, purge_err)) {
            // 期间被用户恢复：跳过即可，不影响其他过期项。
            if (purge_err == "not_found") {
                continue;
            }
            err = purge_err;
            return false;
        }
        out_shas.insert(out_shas.end(), shas.begin(), shas.end());
    }
    return true;
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
    err.clear();
    out_file_id = 0;
    sqlite3* h = handle();
    if (h == nullptr) {
        err = "数据库未打开";
        return false;
    }

    // 不区分 deleted_at：进回收站但未彻底删除的内容仍算该账户已拥有（PRD R9/R10）。
    const char* sql =
        "SELECT f.id FROM files f JOIN blobs b ON b.id = f.blob_id"
        " WHERE f.owner_id = ? AND f.kind = 'file' AND b.sha256 = ?"
        " ORDER BY f.id LIMIT 1";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(h, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        err = sqlite3_errmsg(h);
        return false;
    }
    sqlite3_bind_int64(stmt, 1, owner_id);
    sqlite3_bind_text(stmt, 2, sha256.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        out_file_id = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        err = sqlite3_errmsg(h);
        return false;
    }
    return out_file_id != 0;
}

bool create_file_entry(long long owner_id, long long parent_id, const std::string& desired_name,
                       long long blob_id, long long size, long long& out_id,
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
        " updated_at) VALUES(?, ?, ?, 'file', ?, ?, NULL, ?, ?)";

    // 与 create_folder 一致：直接插入并依赖唯一索引，冲突就换下一个候选名。
    for (int i = 0; i < 100; ++i) {
        const std::string candidate = make_candidate(desired_name, i);
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(h, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            err = sqlite3_errmsg(h);
            return false;
        }
        sqlite3_bind_int64(stmt, 1, owner_id);
        sqlite3_bind_int64(stmt, 2, parent_id);
        sqlite3_bind_text(stmt, 3, candidate.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 4, size);
        sqlite3_bind_int64(stmt, 5, blob_id);
        sqlite3_bind_int64(stmt, 6, now);
        sqlite3_bind_int64(stmt, 7, now);

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

}  // namespace files
}  // namespace db
