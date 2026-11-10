#include "db/db.h"

#include <ctime>

#include "db/schema.h"

namespace db {

namespace {

sqlite3* g_db = nullptr;

bool exec_sql(const char* sql, std::string& err) {
    char* message = nullptr;
    if (sqlite3_exec(g_db, sql, nullptr, nullptr, &message) != SQLITE_OK) {
        err = message != nullptr ? message : "SQL 执行失败";
        if (message != nullptr) {
            sqlite3_free(message);
        }
        return false;
    }
    return true;
}

}  // namespace

bool open(const std::string& path, std::string& err) {
    err.clear();
    close();

    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
    if (sqlite3_open_v2(path.c_str(), &g_db, flags, nullptr) != SQLITE_OK) {
        err = "打开数据库失败：" + std::string(g_db != nullptr ? sqlite3_errmsg(g_db) : "未知错误");
        close();
        return false;
    }

    // 并发注册等场景下，短暂锁等待比立刻失败更合适。
    sqlite3_busy_timeout(g_db, 5000);

    if (!exec_sql("PRAGMA foreign_keys = ON;", err)) {
        close();
        return false;
    }
    // 内存库不支持 WAL。
    if (path != ":memory:" && !exec_sql("PRAGMA journal_mode = WAL;", err)) {
        close();
        return false;
    }
    return true;
}

void close() {
    if (g_db != nullptr) {
        sqlite3_close(g_db);
        g_db = nullptr;
    }
}

sqlite3* handle() {
    return g_db;
}

bool init_schema(std::string& err) {
    err.clear();
    if (g_db == nullptr) {
        err = "数据库未打开";
        return false;
    }
    return exec_sql(schema_sql(), err);
}

long long now_seconds() {
    return static_cast<long long>(std::time(nullptr));
}

}  // namespace db
