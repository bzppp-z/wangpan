#include <cstdio>
#include <string>
#include <vector>

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

static long long scalar_int(const std::string& sql) {
    sqlite3_stmt* stmt = nullptr;
    long long value = -1;
    if (sqlite3_prepare_v2(db::handle(), sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            value = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    return value;
}

static bool exec_sql(const std::string& sql) {
    return sqlite3_exec(db::handle(), sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK;
}

static const db::files::TrashRow* trash_find(const std::vector<db::files::TrashRow>& rows,
                                             long long id) {
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].id == id) {
            return &rows[i];
        }
    }
    return nullptr;
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
    expect(db::blobs::incref(blob_id, err), "incref blob for first entry");
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

    // ===== 软删除 / 回收站 / 恢复 / 彻底删除 =====
    const std::string sha_b(64, 'b');
    long long blob_b = 0;
    expect(db::blobs::get_or_create(sha_b, 100, blob_b, err), "create blob b");

    long long folder_a = 0;
    std::string folder_a_name;
    expect(db::files::create_folder(alice, 0, "A", folder_a, folder_a_name, err), "create A");
    long long file_f = 0;
    std::string file_f_name;
    expect(db::blobs::incref(blob_b, err), "incref blob b");
    expect(db::files::create_file_entry(alice, folder_a, "f.bin", blob_b, 100, file_f, file_f_name,
                                        err),
           "create A/f.bin");

    const long long used_before = db::used_bytes(alice);
    expect(db::files::soft_delete_subtree(alice, folder_a, err), "soft delete A");
    expect(scalar_int("SELECT COUNT(*) FROM files WHERE id IN (" + std::to_string(folder_a) + "," +
                      std::to_string(file_f) + ") AND deleted_at IS NOT NULL") == 2,
           "subtree marked deleted");
    expect(db::used_bytes(alice) == used_before, "soft delete keeps used_bytes");

    std::vector<db::files::TrashRow> trash;
    expect(db::files::list_trash(alice, trash, err), "list trash");
    // 前面的重命名用例故意留下了一条已软删除的 a.txt (1)，这里只针对 A 子树断言。
    expect(trash_find(trash, file_f) == nullptr, "trash lists only subtree root");
    expect(trash_find(trash, folder_a) != nullptr, "trash root is A");
    expect(trash_find(trash, folder_a) != nullptr &&
               trash_find(trash, folder_a)->parent_name.empty(),
           "trash root parent name empty");
    expect(trash_find(trash, folder_a) != nullptr &&
               trash_find(trash, folder_a)->expires_at ==
                   trash_find(trash, folder_a)->deleted_at + 30LL * 24 * 3600,
           "trash expires in 30 days");
    expect(db::files::list_trash(bob, trash, err) && trash.empty(), "bob trash empty");

    // 恢复时根目录已有同名 A：按 R2 得到 "A (1)"，子项一并恢复。
    long long folder_a2 = 0;
    std::string folder_a2_name;
    expect(db::files::create_folder(alice, 0, "A", folder_a2, folder_a2_name, err), "create new A");
    std::string restored_name;
    long long restored_parent = -1;
    expect(db::files::restore_subtree(alice, folder_a, restored_name, restored_parent, err),
           "restore A");
    expect(restored_name == "A (1)", "restore renamed to A (1)");
    expect(restored_parent == 0, "restore to root");
    expect(scalar_int("SELECT COUNT(*) FROM files WHERE id IN (" + std::to_string(folder_a) + "," +
                      std::to_string(file_f) + ") AND deleted_at IS NULL") == 2,
           "subtree restored");
    expect(db::files::list_trash(alice, trash, err) && trash_find(trash, folder_a) == nullptr,
           "A removed from trash after restore");

    // 再次删除后彻底删除：files 行消失、blob 引用归零并删除 blobs 行。
    expect(db::files::soft_delete_subtree(alice, folder_a, err), "soft delete A again");
    std::vector<std::string> purged_shas;
    expect(db::files::purge_subtree(alice, folder_a, purged_shas, err), "purge A");
    expect(scalar_int("SELECT COUNT(*) FROM files WHERE id IN (" + std::to_string(folder_a) + "," +
                      std::to_string(file_f) + ")") == 0,
           "purged files rows gone");
    expect(purged_shas.size() == 1 && purged_shas[0] == sha_b, "purge returns blob sha");
    expect(scalar_int("SELECT COUNT(*) FROM blobs WHERE id = " + std::to_string(blob_b)) == 0,
           "zero ref blob row removed");

    // 恢复不存在的条目 / 非子树根都返回 not_found。
    std::string gone_name;
    long long gone_parent = 0;
    expect(!db::files::restore_subtree(alice, folder_a, gone_name, gone_parent, err),
           "restore purged rejected");
    expect(err == "not_found", "restore purged err not_found");

    // ===== 30 天过期清理 =====
    const std::string sha_c(64, 'c');
    long long blob_c = 0;
    expect(db::blobs::get_or_create(sha_c, 10, blob_c, err), "create blob c");
    long long folder_e = 0;
    std::string folder_e_name;
    expect(db::files::create_folder(alice, 0, "E", folder_e, folder_e_name, err), "create E");
    long long file_g = 0;
    std::string file_g_name;
    expect(db::blobs::incref(blob_c, err), "incref blob c");
    expect(db::files::create_file_entry(alice, folder_e, "g.bin", blob_c, 10, file_g, file_g_name,
                                        err),
           "create E/g.bin");
    expect(db::files::soft_delete_subtree(alice, folder_e, err), "soft delete E");

    const long long now = db::now_seconds();
    const long long past = now - 31LL * 24 * 3600;
    expect(exec_sql("UPDATE files SET deleted_at = " + std::to_string(past) + " WHERE id IN (" +
                    std::to_string(folder_e) + "," + std::to_string(file_g) + ")"),
           "age E by 31 days");
    std::vector<std::string> expired_shas;
    expect(db::files::purge_expired(now, expired_shas, err), "purge expired");
    expect(scalar_int("SELECT COUNT(*) FROM files WHERE id IN (" + std::to_string(folder_e) + "," +
                      std::to_string(file_g) + ")") == 0,
           "expired subtree removed");
    expect(expired_shas.size() == 1 && expired_shas[0] == sha_c, "expired returns blob sha");
    expect(scalar_int("SELECT COUNT(*) FROM blobs WHERE id = " + std::to_string(blob_c)) == 0,
           "expired blob row removed");
    expect(db::files::list_trash(alice, trash, err) && trash_find(trash, folder_e) == nullptr,
           "E removed from trash after expiry");

    // 未过期的条目不会被清理。
    long long folder_keep = 0;
    std::string folder_keep_name;
    expect(db::files::create_folder(alice, 0, "keep", folder_keep, folder_keep_name, err),
           "create keep");
    expect(db::files::soft_delete_subtree(alice, folder_keep, err), "soft delete keep");
    expect(db::files::purge_expired(now, expired_shas, err), "purge expired again");
    expect(expired_shas.empty(), "fresh trash kept");

    db::close();
    std::printf("trash: %s (%d failed)\n", g_fail ? "FAILED" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
