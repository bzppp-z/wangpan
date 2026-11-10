#include <cstdio>
#include <string>

#include <sqlite3.h>

#include "db/blobs.h"
#include "db/db.h"
#include "db/files.h"
#include "db/users.h"

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

    // ===== 重命名 =====
    long long file_a = 0;
    std::string name;
    expect(db::files::create_file_entry(alice, 0, "a.txt", blob_id, 10, file_a, name, err),
           "create a.txt");
    expect(name == "a.txt", "a.txt name kept");

    std::string final_name;
    expect(db::files::rename_file(alice, file_a, "b.txt", final_name, err), "rename a.txt -> b.txt");
    expect(final_name == "b.txt", "rename result b.txt");

    long long file_a2 = 0;
    expect(db::blobs::incref(blob_id, err), "incref blob");
    expect(db::files::create_file_entry(alice, 0, "a.txt", blob_id, 10, file_a2, name, err),
           "create second a.txt");
    expect(name == "a.txt", "second a.txt kept");

    // 改名撞上同目录已有名称：按 R2 得到 "a.txt (1)"（后缀加在完整名称之后）。
    expect(db::files::rename_file(alice, file_a, "a.txt", final_name, err),
           "rename b.txt -> a.txt");
    expect(final_name == "a.txt (1)", "conflict renamed to a.txt (1)");

    // 名称未变化：直接成功且不改名成 "(1)"。
    expect(db::files::rename_file(alice, file_a2, "a.txt", final_name, err),
           "rename to same name");
    expect(final_name == "a.txt", "same name kept");

    // 越权与不存在统一失败，err 为 not_found。
    std::string cross_err;
    expect(!db::files::rename_file(bob, file_a, "hijack.txt", final_name, cross_err),
           "cross account rename rejected");
    expect(cross_err == "not_found", "cross account err not_found");
    expect(!db::files::rename_file(alice, 999999, "x.txt", final_name, cross_err),
           "rename missing rejected");
    expect(cross_err == "not_found", "missing err not_found");

    // 已软删除的条目不可重命名（直接改库模拟回收站状态）。
    const std::string sql = "UPDATE files SET deleted_at=1 WHERE id=" + std::to_string(file_a2);
    sqlite3_exec(db::handle(), sql.c_str(), nullptr, nullptr, nullptr);
    expect(!db::files::rename_file(alice, file_a2, "gone.txt", final_name, cross_err),
           "rename trashed rejected");
    expect(cross_err == "not_found", "trashed err not_found");

    // 文件夹也可重命名。
    long long folder_id = 0;
    std::string folder_name;
    expect(db::files::create_folder(alice, 0, "文档", folder_id, folder_name, err),
           "create folder");
    expect(db::files::rename_file(alice, folder_id, "资料", final_name, err), "rename folder");
    expect(final_name == "资料", "folder renamed");

    db::close();
    std::printf("trash: %s (%d failed)\n", g_fail ? "FAILED" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
