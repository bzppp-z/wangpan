#include <cstdio>
#include <string>
#include <vector>

#include "db/db.h"
#include "db/users.h"
#include "db/files.h"

static int g_fail = 0;

static void expect(bool ok, const char* name) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) {
        ++g_fail;
    }
}

// 统计指定名称的表是否存在。
static bool table_exists(const char* table) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name=?";
    if (sqlite3_prepare_v2(db::handle(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, table, -1, SQLITE_TRANSIENT);
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        found = sqlite3_column_int(stmt, 0) == 1;
    }
    sqlite3_finalize(stmt);
    return found;
}

int main() {
    std::string err;

    expect(db::open(":memory:", err), "open memory db");
    expect(db::init_schema(err), "init schema");

    expect(table_exists("users"), "table users exists");
    expect(table_exists("sessions"), "table sessions exists");
    expect(table_exists("blobs"), "table blobs exists");
    expect(table_exists("files"), "table files exists");
    expect(table_exists("upload_sessions"), "table upload_sessions exists");

    long long user_id = 0;
    expect(db::create_user("alice", "salt", "hash", 5368709120LL, user_id, err),
           "create user alice");
    expect(user_id > 0, "alice id > 0");

    err.clear();
    long long dup_id = 0;
    expect(!db::create_user("alice", "salt", "hash", 5368709120LL, dup_id, err),
           "duplicate username rejected");
    expect(!err.empty(), "duplicate username has error message");

    // 建表可重复执行
    expect(db::init_schema(err), "init schema is idempotent");

    db::close();
    std::printf("schema: %s (%d failed)\n", g_fail ? "FAILED" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
