# 阶段 3：上传（分片、断点续传、秒传、配额）

**目标：** 任意大小文件可上传；中断后只补缺失分片；同一账户已有同内容时秒传；超配额/超上限被拒绝；落盘内容与 SHA-256 完全一致。
**前置条件：** 阶段 1（服务端账户/目录）与阶段 2（Qt 客户端登录/列表）完成。
**产出：** 可上传的客户端、`POST /api/upload/*` 接口、`server/tests/test_storage.cpp`、`client/tests/test_transfer.exe`、上传相关冒烟断言。

**本阶段界面决策：** 不做新的独立窗口。上传入口沿用阶段 2 的主窗口工具栏（把禁用按钮启用），进度先用状态栏摘要呈现；按 Q3 的"底部摘要 + 可展开传输列表"，**展开列表属于阶段 5**，本阶段不实现。本阶段 UI 开工前，仍先用 HTML 草图确认"状态栏摘要长什么样"，确认后再写代码。

**范围边界：** 不做下载（阶段 4）、不做传输面板与持久化队列（阶段 5）。本阶段并发上传固定为 1 个任务（阶段 5 提到 3）。

---

## 存储与数据约定（先定死，后面所有任务都按这个来）

| 项目 | 约定 |
|---|---|
| 分片大小 | 4 MiB（`4194304`），由服务端决定并下发，客户端不得自选 |
| 单文件上限 | 10 GiB（`10737418240`） |
| 上传会话有效期 | 7 天（PRD R4）；过期后 `init` 视为全新会话 |
| 会话目录 | `<data_dir>/tmp/<session_id>/`，分片文件名为 `<index>.chunk`（index 从 0 开始，不补零） |
| 内容落盘 | `<data_dir>/blob/<sha[0:2]>/<sha[2:4]>/<sha256>`，sha 为小写 64 位十六进制 |
| 分片写入 | 先写 `<index>.chunk.tmp`，`fsync` 后 `rename` 为 `<index>.chunk`，避免半截文件被当成已收分片 |
| "已收分片"来源 | 直接扫描会话目录，不额外建表；扫描时校验每个文件的大小，大小不对的分片视为未收 |
| 会话 id | `random_hex(16)` → 32 位小写十六进制，路径只由服务端生成 |
| 秒传判定 | 仅当**该账户自身**已有相同 sha256 的文件条目（含已进回收站、尚未彻底删除的条目）时成立（PRD R10）；秒传仍然创建**新条目**并计入配额 |
| 配额判定 | `used_bytes(owner_id) + size > quota_bytes` → 507；创建条目成功前和合并完成前各校验一次 |

**新增索引（阶段 1 的 `init_schema` 追加，幂等，老库重启自动生效）：**

```sql
CREATE INDEX IF NOT EXISTS idx_upload_sessions_resume
  ON upload_sessions(owner_id, parent_id, sha256, size);
```

---

## 接口契约

| 方法与路径 | 请求 | 成功 | 失败 |
|---|---|---|---|
| `POST /api/upload/init` | JSON `{"parent_id":N,"name":"...","size":S,"sha256":"<64hex>"}` | 秒传：201 `{"instant":true,"file_id":N,"name":"最终名称","sha256":"...","size":S}`；新建/续传：200 `{"instant":false,"session_id":"...","chunk_size":4194304,"chunk_count":N,"received":[0,1,...],"expires_at":T}` | 400 `invalid_name` / `invalid_size` / `file_too_large` / `sha256_invalid`；404 `not_found`（父目录不存在或不属于该账户）；507 `quota_exceeded` |
| `PUT /api/upload/{session_id}/chunks/{index}` | `Content-Type: application/octet-stream`，body 为分片原始字节 | 204 无响应体 | 400 `invalid_chunk` / `chunk_size_mismatch`；404 `session_not_found`（含别人的会话，按不存在处理）；401 |
| `GET /api/upload/{session_id}` | 无 body | 200 `{"session_id","size","chunk_size","chunk_count","received":[...],"expires_at"}` | 404 `session_not_found` |
| `POST /api/upload/{session_id}/complete` | 无 body | 201 `{"file_id":N,"name":"最终名称","sha256":"...","size":S,"dedup":true/false}` | 400 `checksum_mismatch` / `size_mismatch`；404 `session_not_found`；507 `quota_exceeded` |
| `DELETE /api/upload/{session_id}` | 无 body | 204（删除会话记录与临时分片） | 404 `session_not_found` |

**续传语义：** 客户端再次对"同一 parent_id + 同一名称 + 同一 size + 同一 sha256"调用 `init`，服务端在 7 天内找到同一条会话且目录存在时，返回**同一个 session_id** 与 `received`；客户端跳过 `received` 中的分片。名称、大小、sha 任一变化都视为新任务。

**分片长度规则：** 第 `i` 片长度 = `min(chunk_size, size - i * chunk_size)`；最后一片可以小于 4 MiB；`size == 0` 时 `chunk_count == 0`，直接 `complete`（空内容的 SHA-256 由客户端算出）。

---

## Task 1：服务端存储层与单元测试

**目标：** 分片落盘、扫描、合并、哈希、提交 blob 这套逻辑独立可测，不依赖 HTTP。

**文件：**
- Create：`server/src/storage/blob.h`、`server/src/storage/blob.cpp`
- Create：`server/tests/test_storage.cpp`
- Modify：`server/CMakeLists.txt`（加入 `src/storage/blob.cpp`、`test_storage` 目标）
- Modify：`server/src/config.h`、`config.cpp`（新增 `--quota-bytes` 参数，默认 5 GiB；供 Task 6 用小配额实例验证）

**提交拆分（本任务 2 个提交）：**
1. `test(storage): 分片扫描/合并/哈希用例｜先红`：提交 `test_storage.cpp`、`blob.h` 与可编译桩（`merge_and_hash` 返回 false）、CMake 目标；预期 `ctest` 中 `storage` FAIL，工程仍可编译。
2. `phase3: 分片落盘与合并｜服务端自算 SHA-256，原子 rename 防半截文件`：实现 `blob.cpp` 与配置参数；预期 `ctest` 全绿。

**函数签名（后续任务直接调用，不再改名）：**

```cpp
namespace storage {
  std::string blob_path(const std::string& data_dir, const std::string& sha256);
  bool write_chunk(const std::string& session_dir, int index,
                   const std::string& bytes, std::int64_t expected_len, std::string& err);
  bool scan_received(const std::string& session_dir, int chunk_count,
                     std::int64_t chunk_size, std::int64_t total_size,
                     std::vector<int>& out_indices, std::string& err);
  bool merge_and_hash(const std::string& session_dir, int chunk_count,
                      std::int64_t chunk_size, std::int64_t total_size,
                      const std::string& out_path, std::string& out_sha256, std::string& err);
  bool commit_blob(const std::string& temp_path, const std::string& data_dir,
                   const std::string& sha256, std::string& err);
  bool remove_tree(const std::string& path);
}
```

**步骤：**

- [x] 先写 `server/tests/test_storage.cpp`（此时应编译失败）：
  - 在临时目录写入 2 个分片（第 0 片全 `A` 4 MiB，第 1 片 1 MiB 全 `B`）；
  - `scan_received` 返回 `{0,1}`；
  - 删除第 1 片后再扫描返回 `{0}`；
  - `merge_and_hash` 输出的 sha 与 `crypto::sha256_hex(完整内容)` 一致，且 `out_path` 大小等于 5 MiB；
  - `commit_blob` 后 `blob_path(data_dir, sha)` 存在，内容与合并结果一致；
  - 同 sha 再提交一次（模拟两个账户同时传同一内容）不报错、不破坏已有文件。
- [x] 实现 `blob.cpp`：
  - `write_chunk` 校验 `bytes.size() == expected_len`，写 `.tmp` → `rename`；
  - `scan_received` 从 0 到 `chunk_count-1` 检查 `<i>.chunk` 是否存在且大小等于该片应有长度；只读元信息，不读内容；
  - `merge_and_hash` 顺序读分片、拼接写入 `out_path`，同时用 OpenSSL `EVP_MD_CTX` 流式计算 SHA-256；任何分片缺失或大小不符返回错误；
  - `commit_blob`：目标路径已存在且大小一致 → 删除临时文件直接成功；否则 `rename`（跨目录时先 `rename` 到同目录临时名再落位）。
- [x] 在 `CMakeLists.txt` 增加测试目标并跑：

```bash
cmake --build build-wsl -j
ctest --test-dir build-wsl --output-on-failure
```

  - 预期：`schema` 与 `storage` 两个测试都 Passed。
  - 实测：`schema` / `auth` / `storage` 三个测试全 Passed，`test_storage` 24 条断言 0 失败（额外覆盖了「长度不符的分片被拒绝且不留文件」「截断分片视为未收」「缺片合并失败并删除半截输出」）。

**安全 / 边界说明：** 会话目录名与 blob 路径全部来自服务端生成；任何客户端字符串都不参与路径拼接。临时文件落在 `<data_dir>` 内，避免跨文件系统 `rename` 失败。

---

## Task 2：上传初始化（秒传、续传、配额）

**目标：** `init` 一次调用就能决定"秒传 / 新会话 / 续传"，并拒绝超配额与超上限。

**文件：**
- Create：`server/src/api/upload.h`、`server/src/api/upload.cpp`（先只实现 init）
- Create：`server/tests/test_instant.cpp`
- Modify：`server/CMakeLists.txt`（新增 `test_instant` 目标）
- Modify：`server/src/db/schema.h`（追加上面的 resume 索引）
- Modify：`server/src/db/files.h`、`files.cpp`（新增 `find_owned_by_sha(owner_id, sha, out_file_id, err)`、`create_file_entry(owner_id, parent_id, desired_name, blob_id, size, out_id, out_name, err)`）
- Modify：`server/src/db/blobs.h`、`blobs.cpp`（新增 `get_or_create(sha, size, out_id, err)`、`incref(blob_id, err)`）
- Modify：`server/src/main.cpp`（注册路由）
- Modify：`scripts/smoke.sh`（追加 init 用例）

**提交拆分（本任务 2 个提交）：**
1. `test(instant): 秒传归属判定用例｜先红`：提交 `test_instant.cpp`、`files.h` / `blobs.h` 声明与桩（`find_owned_by_sha` 恒返回 false）、CMake 目标；预期 `ctest` 中 `instant` FAIL，其余测试仍 PASS。
2. `phase3: 上传初始化与秒传｜只认本账户已有内容，防跨账户白拿`：实现数据访问与 `init` 路由、冒烟断言、计划勾选；预期 `ctest` 全绿。

**步骤：**

- [x] 先写 `server/tests/test_instant.cpp`（配套桩，预期 `alice owns` 起失败）：

```cpp
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
    if (!ok) ++g_fail;
}

int main() {
    std::string err;
    db::open(":memory:", err);
    db::init_schema(err);

    long long alice = 0, bob = 0;
    db::create_user("alice", "salt", "hash", 5368709120LL, alice, err);
    db::create_user("bob",   "salt", "hash", 5368709120LL, bob, err);

    const std::string sha(64, 'a');
    long long blob_id = 0;
    db::blobs::get_or_create(sha, 10, blob_id, err);

    long long file_id = 0;
    std::string final_name;
    db::files::create_file_entry(alice, 0, "a.bin", blob_id, 10, file_id, final_name, err);

    long long found = 0;
    expect(db::files::find_owned_by_sha(alice, sha, found, err), "alice owns same content");
    expect(found == file_id, "alice matched file id");
    expect(!db::files::find_owned_by_sha(bob, sha, found, err), "bob does not own content");

    // 回收站中的条目仍算"已拥有"（PRD R9/R10）
    const std::string sql = "UPDATE files SET deleted_at=" + std::to_string(std::time(nullptr)) +
                            " WHERE id=" + std::to_string(file_id);
    sqlite3_exec(db::handle(), sql.c_str(), nullptr, nullptr, nullptr);
    expect(db::files::find_owned_by_sha(alice, sha, found, err), "trashed file still owned");

    db::close();
    std::printf("instant: %s (%d failed)\n", g_fail ? "FAILED" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
```

- [x] 在 `CMakeLists.txt` 追加（与其它测试并列）：

```cmake
add_executable(test_instant tests/test_instant.cpp
  src/db/db.cpp src/db/users.cpp src/db/blobs.cpp src/db/files.cpp)
target_include_directories(test_instant PRIVATE src third_party)
target_link_libraries(test_instant PRIVATE SQLite::SQLite3 Threads::Threads)
add_test(NAME instant COMMAND test_instant)
```

- [x] 跑 `ctest`，确认 `instant` Failed 后提交第 1 个 commit，再追加 `scripts/smoke.sh` 用例（沿用阶段 1 的 `$token`，文件用 5 MiB 随机内容）：

```bash
dd if=/dev/urandom of=/tmp/wp_big.bin bs=1M count=5 status=none
sha=$(sha256sum /tmp/wp_big.bin | cut -d' ' -f1)
init=$(curl -s -o /tmp/wp_init.json -w '%{http_code}' -X POST "$BASE/api/upload/init" \
  -H "Authorization: Bearer $token" -H 'Content-Type: application/json' \
  -d "{\"parent_id\":0,\"name\":\"big.bin\",\"size\":5242880,\"sha256\":\"$sha\"}")
check "upload init 200" 200 "$init"
sid=$(sed -n 's/.*"session_id":"\([0-9a-f]*\)".*/\1/p' /tmp/wp_init.json)
check_grep "init chunk_count 2" /tmp/wp_init.json '"chunk_count":2'
badsha=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/api/upload/init" \
  -H "Authorization: Bearer $token" -H 'Content-Type: application/json' \
  -d '{"parent_id":0,"name":"x.bin","size":1,"sha256":"zzz"}')
check "invalid sha 400" 400 "$badsha"
```

- [x] 实现 `POST /api/upload/init`：
  1. 鉴权 → 校验 `parent_id` 归属（404）；
  2. 校验名称（复用阶段 1 规则）、`size`（`0 <= size <= 10 GiB`）、`sha256`（正则 `^[0-9a-f]{64}$`）；
  3. 配额：`used_bytes + size > quota_bytes` → 507 `quota_exceeded`；
  4. 秒传：查"该账户任一 `files` 行（不区分 `deleted_at`、`kind='file'`）的 `blob_id` 对应此 sha"；命中 → 按 R2 取唯一名称、插入新条目、`blobs.refcount + 1` → 201 `instant=true`；
  5. 续传：查 `upload_sessions` 中 `owner_id/parent_id/name/size/sha256` 相同、`created_at >= now - 7d`、且会话目录存在 → 直接返回该会话与 `received`；
  6. 否则删除该账户下同 sha 的过期会话，生成 `session_id`、建目录、插入 `upload_sessions` → 200。
- [x] 运行 `ctest`，确认 `instant` 转绿；重跑冒烟，确认 `upload init 200`、`init chunk_count 2`、`invalid sha 400` 三条 PASS。
  - 实测：`schema` / `auth` / `storage` / `instant` 四个测试全 Passed；冒烟 38 条全 PASS（新增 10 条：会话 id 32 位、chunk_count 2、instant=false、received 为空、重复 init 返回同一会话、sha 非法 400、超上限 400、超配额 507 等）。
- [x] 提交第 2 个 commit（实现 + 冒烟 + 计划勾选），然后 push。

**安全 / 边界说明：** 客户端提交的 `sha256` **只**用于"判断自己是否已拥有"和"合并后比对"，绝不因为"服务器上存在该 sha 的 blob"就跳过上传——否则任何账号都能凭 sha 白拿别人的文件（PRD R10）。

---

## Task 3：分片上传、状态查询、合并完成

**目标：** 分片可重复上传；合并时服务端自己算 SHA-256；校验通过才生成文件条目；失败自动清理会话。

**文件：**
- Modify：`server/src/api/upload.cpp`（分片、状态、完成、取消）
- Modify：`server/src/main.cpp`（注册路由、`set_payload_max_length(8 * 1024 * 1024)`）
- Modify：`scripts/smoke.sh`（追加分片与断点续传用例）

**步骤：**

- [ ] 追加冒烟用例（先传第 0 片 → 查状态 → 重新 init 验证续传 → 补齐第 1 片 → 完成）：

```bash
split -b 4194304 -d -a 1 /tmp/wp_big.bin /tmp/wp_part_
p0=$(curl -s -o /dev/null -w '%{http_code}' -X PUT "$BASE/api/upload/$sid/chunks/0" \
  -H "Authorization: Bearer $token" -H 'Content-Type: application/octet-stream' \
  --data-binary @/tmp/wp_part_0)
check "chunk 0 204" 204 "$p0"
st=$(curl -s "$BASE/api/upload/$sid" -H "Authorization: Bearer $token")
echo "$st" > /tmp/wp_status.json
check_grep "received has 0" /tmp/wp_status.json '"received":[0]'
reinit=$(curl -s -o /tmp/wp_reinit.json -w '%{http_code}' -X POST "$BASE/api/upload/init" \
  -H "Authorization: Bearer $token" -H 'Content-Type: application/json' \
  -d "{\"parent_id\":0,\"name\":\"big.bin\",\"size\":5242880,\"sha256\":\"$sha\"}")
check "re-init 200" 200 "$reinit"
check_grep "re-init same session" /tmp/wp_reinit.json "$sid"
p1=$(curl -s -o /dev/null -w '%{http_code}' -X PUT "$BASE/api/upload/$sid/chunks/1" \
  -H "Authorization: Bearer $token" -H 'Content-Type: application/octet-stream' \
  --data-binary @/tmp/wp_part_1)
check "chunk 1 204" 204 "$p1"
done=$(curl -s -o /tmp/wp_done.json -w '%{http_code}' -X POST "$BASE/api/upload/$sid/complete" \
  -H "Authorization: Bearer $token")
check "complete 201" 201 "$done"
check_grep "complete size" /tmp/wp_done.json '"size":5242880'
```

- [ ] 实现 `PUT .../chunks/{index}`：校验会话归属与有效期 → 校验 `index < chunk_count` → 校验 body 长度等于该片应有长度 → `storage::write_chunk`（重复上传覆盖）→ 204。
- [ ] 实现 `GET /api/upload/{sid}`：归属校验 → `scan_received` → 200。
- [ ] 实现 `POST .../complete`：
  1. 归属与有效期校验；`scan_received` 后若缺分片 → 400 `invalid_chunk`（提示缺第几片）；
  2. `merge_and_hash` 到 `<data_dir>/tmp/<sid>/merged.tmp`；算出的 sha 与 `upload_sessions.sha256` 不符 → 删除会话目录与记录 → 400 `checksum_mismatch`；
  3. 实际大小与声明不符 → 400 `size_mismatch`，同样清理；
  4. 事务内：配额复查 → 按 sha 查/建 blob（`INSERT ... ON CONFLICT(sha256) DO NOTHING` 后再查）→ `refcount + 1` → 按 R2 创建文件条目 → 删除 `upload_sessions` 行；
  5. `commit_blob` 落盘 → 删除会话目录 → 201。
- [ ] 实现 `DELETE /api/upload/{sid}`：删除记录与目录 → 204。
- [ ] 追加"取消后状态 404"与"错误校验值 400"两条断言，重跑冒烟全绿。

**安全 / 边界说明：** 合并结果必须由服务端计算哈希，客户端的 sha 只做比对；校验失败一律丢弃会话，避免半成品被反复重试写坏磁盘。注册的 blob 行与磁盘文件在极少数崩溃场景可能不一致，启动时不修复，留到阶段 5 的清理任务里只删除"无引用且无文件"的行。

---

## Task 4：客户端上传任务（哈希线程、分片、暂停、重试）

**目标：** 把一个本地文件可靠地传上去：哈希不卡界面、分片顺序发送、暂停后能续、失败能重试。

**文件：**
- Create：`client/src/transfer/transferutil.h`、`transferutil.cpp`（`chunkCount`、`chunkLength`、`formatSize`、`formatSpeed`、`etaSeconds`）
- Create：`client/src/transfer/hashworker.h`、`hashworker.cpp`（`QThread` 子类，1 MiB 分块读，`progress(int)`、`done(QString sha)`、`cancel()`）
- Create：`client/src/transfer/uploadtask.h`、`uploadtask.cpp`
- Create：`client/tests/test_transfer.pro`、`test_transfer.cpp`
- Modify：`client/src/apiclient.h`、`apiclient.cpp`（新增 `QNetworkAccessManager* manager()`、`QNetworkRequest requestFor(const QString& path, const QString& contentType = "application/json") const`、`QString token() const`；`requestFor` 自动带 `Authorization: Bearer`）
- Modify：`client/wangpan.pro`（把上面新增的源文件逐个加入）

**步骤：**

- [ ] 先写 `client/tests/test_transfer.cpp`（纯函数，无网络）：

```cpp
#include <QtTest>
#include "transferutil.h"

class TestTransfer : public QObject {
    Q_OBJECT
private slots:
    void countsChunks() {
        QCOMPARE(TransferUtil::chunkCount(0, 4194304), 0);
        QCOMPARE(TransferUtil::chunkCount(1, 4194304), 1);
        QCOMPARE(TransferUtil::chunkCount(4194304, 4194304), 1);
        QCOMPARE(TransferUtil::chunkCount(4194305, 4194304), 2);
    }
    void computesChunkLength() {
        QCOMPARE(TransferUtil::chunkLength(0, 5242880, 4194304), qint64(4194304));
        QCOMPARE(TransferUtil::chunkLength(1, 5242880, 4194304), qint64(1048576));
    }
    void formatsSize() {
        QCOMPARE(TransferUtil::formatSize(0), QString("0 B"));
        QCOMPARE(TransferUtil::formatSize(1536), QString("1.5 KB"));
        QCOMPARE(TransferUtil::formatSize(5242880), QString("5.0 MB"));
    }
    void computesEta() {
        QCOMPARE(TransferUtil::etaSeconds(0, 0), 0);
        QCOMPARE(TransferUtil::etaSeconds(1048576, 1024), 1024);
    }
};
QTEST_MAIN(TestTransfer)
#include "test_transfer.moc"
```

- [ ] 写 `test_transfer.pro`（`QT += testlib`，`INCLUDEPATH += ../src/transfer`，`SOURCES += test_transfer.cpp ../src/transfer/transferutil.cpp`），跑测试，预期 4 条全过（先失败再实现）。
- [ ] 实现 `UploadTask`：
  - 状态：`Waiting / Hashing / Running / Paused / Verifying / Completed / Failed / Canceled`（与 PRD 3.2 对齐，`Hashing/Verifying` 对应"校验中"）；
  - `start()` → 启动 `HashWorker`（界面显示"校验中 45%"）→ `init` → 若 `instant=true` 直接 `Completed(instant=true)` → 否则按 `received` 过滤后顺序 PUT 分片 → `complete` → 比对服务端返回的 `sha256` 与本地一致；
  - `pause()`：置标志，当前分片响应到达后停，状态 `Paused`；`resume()`：重新 `init`（服务端返回同一 session 与 `received`），从缺失处分片继续；
  - `cancel()`：`DELETE /api/upload/{sid}`，删除任务；
  - 网络错误自动重试 3 次（间隔 1s/2s/4s），仍失败 → `Paused` 并带提示"网络中断，已暂停"；
  - 速度用 3 秒滑动窗口计算，进度信号 `progress(done, total, bytesPerSec)`。
- [ ] `UploadTask` 不直接持有 token 副本，统一通过 `ApiClient::requestFor()` 取头，登出后旧任务应被 `cancel()`。
- [ ] 编译并跑 `test_transfer.exe`，预期 `Totals: 4 passed, 0 failed`。

**安全说明：** 本地路径只用于读文件，不上传给服务端；服务端返回的最终名称才用于界面显示；分片内容不写日志。

---

## Task 5：主窗口接入上传（文件 / 文件夹递归 / 状态栏摘要）

**目标：** 用户能从工具栏或拖拽上传单个/多个文件与整个文件夹；进度实时可见；完成后列表与配额刷新。

**文件：**
- Create：`client/src/ui/uploadcontroller.h`、`uploadcontroller.cpp`（把"本地路径 → 任务队列"的逻辑从窗口里抽出来，便于阶段 5 换成统一队列）
- Modify：`client/src/ui/mainwindow.h`、`mainwindow.cpp`
- Modify：`client/src/ui/theme.h`（新增 `kProgressTrack "#EBEEF5"`、`kSuccess "#00B42A"`）
- Modify：`client/wangpan.pro`

**界面契约（本任务开工前用草图确认）：**

| 项目 | 规格 |
|---|---|
| 上传按钮 | 启用；点击弹出菜单"上传文件"/"上传文件夹"；不在本阶段做上传中禁用 |
| 拖拽 | 文件列表接受拖拽，拖入多文件与文件夹；文件夹按原目录结构递归 |
| 状态栏摘要 | 左侧追加一段：`上传中 1 · 作业.zip 45% · 2.1 MB/s`，文本 `#1F2329` 12px；右侧 120×12 进度条，背景 `#EBEEF5`，进度条 `#2D8CF0`，空闲时整段隐藏 |
| 完成反馈 | 成功后状态栏显示"上传完成：最终名称"3 秒；列表刷新并选中新文件 |
| 失败反馈 | 状态栏红色 `#E64545` 显示映射后的文案 3 秒；任务本身留在上传控制器里可重试（重试入口在阶段 5 的传输面板） |

**步骤：**

- [ ] 递归上传：用 `QDirIterator(path, QDir::Files, QDirIterator::Subdirectories)` 收集文件；对每个文件计算相对目录，按层调用 `createFolder`，把"相对目录 → folder_id"存进 `QHash<QString, qlonglong>`；再逐个创建 `UploadTask`。
- [ ] 顺序执行（本阶段同时只跑 1 个任务），每个任务结束（成功或失败）后启动下一个。
- [ ] 拖拽事件 `dragEnterEvent` 接受 `text/uri-list`，`dropEvent` 把本地路径交给 `UploadController`。
- [ ] 任务成功 → 若当前目录正是该任务的目标目录，则 `listFiles(currentId)` 刷新并选中；失败 → 按 `ApiClient::errorTextFor` 映射 507/400/413。
- [ ] 手工验证：
  - 上传单个 5 MB 文件 → 状态栏进度从 0 到 100，列表出现文件，配额增加 5 MB；
  - 上传含 3 层子目录的文件夹 → 目录结构与本地一致，空文件夹也被创建；
  - 上传过程中点"退出登录" → 当前任务被取消，无崩溃。

**视觉验证：** 截图 `docs/ui-shots/phase3-upload-running.png`、`phase3-upload-done.png`；确认状态栏摘要出现/消失时，右侧配额条位置不跳动。

---

## Task 6：边界与整体验收（配额、校验失败、跨账户、续传、1 GB）

**目标：** 证明这四种坏情况都被正确处理，且大文件不损坏。

**文件：**
- Modify：`client/src/apiclient.cpp`（补齐 507/400 的文案映射）
- Modify：`README` 不在本阶段改；只在 `server/README.md` 补 `--quota-bytes` 说明
- Modify：`scripts/smoke.sh`（追加秒传与跨账户断言）

**步骤：**

- [ ] 秒传（同账户）：用另一个名称对同一 sha 再 `init`：

```bash
ins=$(curl -s -o /tmp/wp_inst.json -w '%{http_code}' -X POST "$BASE/api/upload/init" \
  -H "Authorization: Bearer $token" -H 'Content-Type: application/json' \
  -d "{\"parent_id\":0,\"name\":\"big-copy.bin\",\"size\":5242880,\"sha256\":\"$sha\"}")
check "instant upload 201" 201 "$ins"
check_grep "instant flag" /tmp/wp_inst.json '"instant":true'
```

- [ ] 跨账户不秒传：用 bob 的 token 对同一 sha `init`，断言 `"instant":false` 且返回 `session_id`（R10）。
- [ ] 校验失败：上传第 0 片后，用一个错误字节覆盖 `/tmp/wp_part_1`，`complete` 预期 400 `checksum_mismatch`，且会话目录被清空（`ls /tmp/wp-test/tmp/<sid>` 不存在）。
- [ ] 小配额实例（另开端口，避免污染主测试库）：

```bash
./build-wsl/wangpan_server --port 18081 --data-dir /tmp/wp-quota --quota-bytes 1048576
# 注册 dave 后上传 5 MiB：init 预期 507
```

- [ ] 大文件：`fsutil file createnew C:\Users\bzp20\Desktop\wangpan\test-1gb.bin 1073741824` → 客户端上传 → 完成后在 WSL 里校验：

```bash
sha256sum /tmp/wp-test/blob/*/*/$(sha256sum test-1gb.bin | cut -d' ' -f1) 2>/dev/null || \
  find /tmp/wp-test/blob -type f -size 1073741824c -exec sha256sum {} \;
```

  - 预期：落盘文件 sha 与本地一致；客户端显示大小 1.0 GB。
- [ ] 关掉客户端再打开、对同一文件重新上传 → `init` 返回原 session 与已收分片，进度不归零（服务端 7 天内保留）。
- [ ] 截图 `docs/ui-shots/phase3-quota-error.png`。

**安全 / 边界说明：** 本阶段所有"文件不存在"的分支统一 404，不区分"不存在"与"不是你的"（R0）。

---

## 阶段完成清单

- [ ] 6 个任务全部完成
- [ ] `ctest` 中 `schema`、`auth`、`storage`、`instant` 全过
- [ ] `scripts/smoke.sh` 全绿（含 init、分片、续传、完成、秒传、跨账户）
- [ ] 5 MiB / 1 GB 文件上传后落盘 sha 与本地一致
- [ ] 断点续传：只补缺失分片，进度不归零
- [ ] 秒传仅在同账户成立，跨账户必须真实上传
- [ ] 超配额返回 507 且客户端提示"空间不足"
- [ ] 大文件上传期间界面不卡死（哈希在子线程）
- [ ] `client/tests/test_transfer.exe` 4 条用例全过
- [ ] 截图 3 张归档在 `docs/ui-shots/`

## 回滚

- 服务端：删除 `server/src/api/upload.*`、`server/src/storage/*` 与对应路由即可回到阶段 2 状态；`upload_sessions` 表与 `tmp/` 目录可保留，不影响既有账户与目录。
- 数据：`blob/` 下已提交的内容若不再被引用，删除 `files` 中对应条目后按阶段 5 的 purging 逻辑清理；测试期间直接删除 `/tmp/wp-test` 重新开始。
- 客户端：删除 `client/src/transfer/` 与 `uploadcontroller.*`，恢复工具栏"上传"按钮为禁用。

## 验证命令汇总

```powershell
# 服务端（WSL 窗口 1）
wsl -d Ubuntu -- bash -lc "cd /mnt/c/Users/bzp20/Desktop/wangpan && cmake -S server -B build-wsl -DCMAKE_BUILD_TYPE=Debug && cmake --build build-wsl -j && ctest --test-dir build-wsl --output-on-failure"

# 服务端（WSL 窗口 2，测试实例）
wsl -d Ubuntu -- bash -lc "cd /mnt/c/Users/bzp20/Desktop/wangpan && ./build-wsl/wangpan_server --port 18080 --data-dir /tmp/wp-test"

# 冒烟（WSL 窗口 3）
wsl -d Ubuntu -- bash -lc "cd /mnt/c/Users/bzp20/Desktop/wangpan && bash scripts/smoke.sh http://127.0.0.1:18080"

# 客户端测试与运行
$env:PATH = "D:\20250519\QT\Qt5.12.11\5.12.11\mingw73_64\bin;D:\20250519\QT\Qt5.12.11\Tools\mingw730_64\bin;" + $env:PATH
cd c:\Users\bzp20\Desktop\wangpan\client\tests
qmake test_transfer.pro CONFIG+=release; mingw32-make -j4; .\release\test_transfer.exe

cd c:\Users\bzp20\Desktop\wangpan\client
qmake wangpan.pro CONFIG+=release; mingw32-make -j4; .\release\wangpan.exe
```
