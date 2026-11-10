#include <cstdio>
#include <ctime>
#include <string>
#include <sqlite3.h>

#include "db/db.h"
#include "db/users.h"
#include "db/blobs.h"
#include "db/files.h"

static int g_fail = 0;

static void expect(bool ok, const char* name) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) {
        ++g_fail;
    }
}

int main() {
    std::string err;
    db::open(":memory:", err);
    db::init_schema(err);

    long long alice = 0, bob = 0;
    db::create_user("alice", "salt", "hash", 5368709120LL, alice, err);
    db::create_user("bob", "salt", "hash", 5368709120LL, bob, err);

    const std::string sha(64, 'a');
    long long blob_id = 0;
    expect(db::blobs::get_or_create(sha, 10, blob_id, err), "get_or_create blob");
    expect(blob_id > 0, "blob id > 0");

    long long file_id = 0;
    std::string final_name;
    expect(db::files::create_file_entry(alice, 0, "a.bin", blob_id, 10, file_id, final_name, err),
           "create file entry");
    expect(final_name == "a.bin", "file name kept");
    expect(file_id > 0, "file id > 0");

    // 秒传判定只认本账户自己的内容。
    long long found = 0;
    expect(db::files::find_owned_by_sha(alice, sha, found, err), "alice owns same content");
    expect(found == file_id && found > 0, "alice matched file id");
    expect(!db::files::find_owned_by_sha(bob, sha, found, err), "bob does not own content");

    // 同目录重名时按 R2 自动改名，而不是覆盖。
    long long second_id = 0;
    std::string second_name;
    expect(db::blobs::incref(blob_id, err), "incref blob");
    expect(db::files::create_file_entry(alice, 0, "a.bin", blob_id, 10, second_id, second_name, err),
           "create duplicate name entry");
    expect(second_name == "a.bin (1)", "duplicate renamed to a.bin (1)");
    expect(second_id != file_id, "duplicate is a new row");

    // 回收站中的条目仍算"已拥有"（PRD R9/R10）。
    const std::string sql = "UPDATE files SET deleted_at=" + std::to_string(std::time(nullptr)) +
                            " WHERE id=" + std::to_string(file_id);
    sqlite3_exec(db::handle(), sql.c_str(), nullptr, nullptr, nullptr);
    expect(db::files::find_owned_by_sha(alice, sha, found, err), "trashed file still owned");

    db::close();
    std::printf("instant: %s (%d failed)\n", g_fail ? "FAILED" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
