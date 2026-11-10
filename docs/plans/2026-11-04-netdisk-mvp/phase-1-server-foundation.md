# 阶段 1：服务端骨架与账户 / 目录接口

**目标：** 在 WSL2 中编译出一个可启动的服务端，能自动建库，完成注册、登录、登出、Bearer 鉴权、目录列表、新建文件夹，并用冒烟脚本端到端验证。
**前置条件：** 无（`server/third_party` 头文件、WSL 工具链、SQLite/OpenSSL 头文件已就绪）。
**产出：** `build-wsl/wangpan_server` 可执行文件、`scripts/smoke.sh`、`server/README.md`。

**本阶段实现的界面决策：** 无。界面（Q1–Q4）全部在阶段 2，草图确认前不写任何 Qt 代码。

**范围边界：** 本阶段只做服务端，不产生客户端代码；上传、下载、预览、回收站在阶段 3–5。HTTP 明文与开放注册按 M1/M3 已确认，本阶段不引入 TLS。

---

## Task 1：构建骨架与健康检查

**目标：** 建立 CMake 工程，服务端能编译启动，`GET /api/health` 返回 200。

**文件：**
- Create：`server/CMakeLists.txt`
- Create：`server/src/main.cpp`
- Create：`server/src/config.h`、`server/src/config.cpp`
- Create：`server/src/log.h`
- Create：`server/src/api/respond.h`
- Create：`scripts/smoke.sh`

**步骤：**

- [x] 先写 `scripts/smoke.sh` 的健康检查段落，运行，确认当前失败（服务端还不存在）。

```bash
#!/usr/bin/env bash
# 用法：bash scripts/smoke.sh [BASE_URL]
set -u
BASE="${1:-http://127.0.0.1:18080}"
pass=0; fail=0
check() { # 名称 期望 实际
  if [ "$2" = "$3" ]; then echo "PASS: $1"; pass=$((pass+1));
  else echo "FAIL: $1 (期望 $2，实得 $3)"; fail=$((fail+1)); fi
}
check_grep() { # 名称 文件 模式
  if grep -q "$3" "$2"; then echo "PASS: $1"; pass=$((pass+1));
  else echo "FAIL: $1 (未在 $2 中找到 $3)"; fail=$((fail+1)); fi
}
Health=$(
  curl -s -o /tmp/wp_health.json -w '%{http_code}' --max-time 5 "$BASE/api/health"
)
check "health 200" 200 "$Health"
check_grep "health body" /tmp/wp_health.json '"ok":true'
echo "通过 $pass 失败 $fail"
[ "$fail" -eq 0 ]
```

- [x] 创建 `server/CMakeLists.txt`（C++14，链接 sqlite3 / OpenSSL / pthread，测试用 CTest）：

```cmake
cmake_minimum_required(VERSION 3.16)
project(wangpan_server CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Task 1 只需要两个源文件；Task 2/3/4 完成后，把它新增的 .cpp 逐个加进这个列表
set(APP_SOURCES
  src/main.cpp
  src/config.cpp
)

add_executable(wangpan_server ${APP_SOURCES})
target_include_directories(wangpan_server PRIVATE src third_party)
find_package(SQLite3 REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
target_link_libraries(wangpan_server PRIVATE SQLite::SQLite3 OpenSSL::Crypto Threads::Threads)

enable_testing()
# 测试目标在 Task 2 创建 tests/test_schema.cpp 之后再取消注释
# add_executable(test_schema tests/test_schema.cpp src/db/db.cpp src/db/users.cpp)
# target_include_directories(test_schema PRIVATE src third_party)
# target_link_libraries(test_schema PRIVATE SQLite::SQLite3 Threads::Threads)
# add_test(NAME schema COMMAND test_schema)
```

- [x] 写 `server/src/config.h` / `config.cpp`：`Config` 含 `port`（默认 8080）、`bind_addr`（默认 `127.0.0.1`）、`data_dir`（默认 `./data`）、`chunk_size`（默认 4 MiB）、`quota_bytes`（默认 5 GiB）；`parse_args(argc, argv, Config&, std::string& err)` 支持 `--port`、`--bind`、`--data-dir`。非法端口（1–65535 之外）返回错误。
- [x] 写 `server/src/log.h`：`log_info/log_warn/log_error` 输出 `[时间][级别] 消息` 到 stdout 与 stderr，**不打印密码、token、请求体**。
- [x] 写 `server/src/api/respond.h`：`json_ok(...)`、`json_error(status, code, message)`，错误体统一为 `{"error":"<code>","message":"<中文提示>"}`。
- [x] 写 `server/src/main.cpp`：解析参数 → 创建数据目录 → 注册 `GET /api/health` 返回 `{"ok":true,"version":"0.1"}` → `svr.listen(bind, port)`；启动时打印实际监听地址与数据目录。
- [x] 编译并启动：

```bash
cd /mnt/c/Users/bzp20/Desktop/wangpan
cmake -S server -B build-wsl -DCMAKE_BUILD_TYPE=Debug
cmake --build build-wsl -j
./build-wsl/wangpan_server --port 18080 --data-dir /tmp/wp-test
```

- [x] 另一个终端运行 `bash scripts/smoke.sh http://127.0.0.1:18080`
  - 预期：`PASS: health 200`、`PASS: health body`，退出码 0。

**安全 / 边界说明：** 默认绑定 `127.0.0.1`，避免调试期间暴露到局域网；服务端不使用 root 权限运行。

---

## Task 2：SQLite 表结构与数据访问层

**目标：** 服务端启动时幂等建表；用户、会话、文件、blob、上传会话五张表可用；数据访问封装在 `db::` 命名空间。

**文件：**
- Create：`server/src/db/schema.h`、`server/src/db/db.h`、`server/src/db/db.cpp`
- Create：`server/src/db/users.h`、`server/src/db/users.cpp`
- Create：`server/src/db/sessions.h`、`server/src/db/sessions.cpp`
- Create：`server/src/db/files.h`、`server/src/db/files.cpp`
- Create：`server/tests/test_schema.cpp`
- Modify：`server/CMakeLists.txt`（把 `db/*.cpp` 加入 `APP_SOURCES`，启用 `test_schema` 目标）
- Modify：`server/src/main.cpp`（启动时打开 `data_dir/wangpan.db` 并幂等建表）

**步骤：**

- [x] 先写 `server/tests/test_schema.cpp`（期望先失败：`init_schema` 尚未实现）。测试内容：
  - `db::open(":memory:", err)` 成功；
  - `db::init_schema(err)` 成功；
  - 查询 `sqlite_master`，五张表 `users/sessions/blobs/files/upload_sessions` 均存在；
  - `db::create_user("alice", "salt", "hash", 5368709120, id, err)` 成功，`id > 0`；
  - 再次 `create_user("alice", ...)` 失败且错误信息非空（唯一约束生效）。
- [x] 写 `schema.h`，用 raw string 内嵌下列 SQL，由 `db::init_schema` 依次执行（`CREATE ... IF NOT EXISTS`，可重复执行）：

```sql
PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS users (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  username TEXT NOT NULL UNIQUE,
  pass_salt TEXT NOT NULL,
  pass_hash TEXT NOT NULL,
  quota_bytes INTEGER NOT NULL,
  created_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS sessions (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  token_hash TEXT NOT NULL UNIQUE,
  user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  expires_at INTEGER NOT NULL,
  created_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_sessions_user ON sessions(user_id);

CREATE TABLE IF NOT EXISTS blobs (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  sha256 TEXT NOT NULL UNIQUE,
  size INTEGER NOT NULL,
  refcount INTEGER NOT NULL DEFAULT 0,
  created_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS files (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  owner_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  parent_id INTEGER NOT NULL DEFAULT 0,
  name TEXT NOT NULL,
  kind TEXT NOT NULL CHECK (kind IN ('file','folder')),
  size INTEGER NOT NULL DEFAULT 0,
  blob_id INTEGER REFERENCES blobs(id),
  deleted_at INTEGER,
  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL
);
CREATE UNIQUE INDEX IF NOT EXISTS idx_files_active_name
  ON files(owner_id, parent_id, name) WHERE deleted_at IS NULL;
CREATE INDEX IF NOT EXISTS idx_files_parent ON files(owner_id, parent_id);

CREATE TABLE IF NOT EXISTS upload_sessions (
  id TEXT PRIMARY KEY,
  owner_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  parent_id INTEGER NOT NULL DEFAULT 0,
  name TEXT NOT NULL,
  size INTEGER NOT NULL,
  sha256 TEXT NOT NULL,
  chunk_size INTEGER NOT NULL,
  chunk_count INTEGER NOT NULL,
  blob_id INTEGER,
  created_at INTEGER NOT NULL
);
```

- [x] 写 `db.h/.cpp`：`open(path, err)`（文件库开启 WAL 与 `foreign_keys`，`:memory:` 跳过 WAL）、`close()`、`handle()`、`init_schema(err)`，所有 SQL 错误统一转成 `err` 字符串，不得抛异常。
- [x] 写 `users.h/.cpp`：
  - `bool create_user(username, salt_hex, hash_hex, quota_bytes, out_id, err)`
  - `bool find_user_by_name(username, out_id, out_salt, out_hash, out_quota, err)`
  - `long long used_bytes(user_id)`：统计该用户所有 `kind='file'` 的 `size` 之和（含回收站内文件，符合 PRD R9）。
- [x] 写 `sessions.h/.cpp`：`create_session(user_id, token_hash, expires_at)`、`find_session(token_hash, out_user_id, out_expires_at, err)`、`delete_session(token_hash)`、`delete_expired_sessions()`。
- [x] 写 `files.h/.cpp`：`list_children(owner_id, parent_id)`、`unique_name(owner_id, parent_id, desired_name)`（按 R2 依次尝试 `name`、`name (1)`、`name (2)`…，超过 255 字符时截断原名保留后缀）、`find_folder(owner_id, folder_id, out_row)`、`create_folder(owner_id, parent_id, name, out_id, out_name, err)`（始终返回最终落地名称；依赖 `idx_files_active_name` 唯一索引，插入冲突时换下一个候选名重试，最多 100 次，不做"先查后插"）。
- [x] 编译并跑测试：

```bash
cmake --build build-wsl -j
ctest --test-dir build-wsl --output-on-failure
```
  - 预期：`1/1 Test #1: schema ... Passed`。

**依赖说明：** Task 3、4 的所有接口都依赖本任务的函数签名，签名一旦落地不再改名。

---

## Task 3：注册 / 登录 / 登出与鉴权

**目标：** 账户体系可用；密码以 PBKDF2 存储；token 7 天有效、多设备共存、登出立即失效；未带 token 的受保护接口返回 401。

**文件：**
- Create：`server/src/util/crypto.h`、`server/src/util/crypto.cpp`
- Create：`server/src/api/auth.h`、`server/src/api/auth.cpp`
- Create：`server/tests/test_auth.cpp`
- Modify：`server/CMakeLists.txt`（新增 `test_auth` 目标）
- Modify：`server/src/main.cpp`（注册路由）
- Modify：`scripts/smoke.sh`（追加账户用例）

**提交拆分（本任务 2 个提交）：**
1. `test(auth): 鉴权与会话用例｜先红`：提交 `test_auth.cpp`、`crypto.h/.cpp` 与 `auth.h/.cpp` 的**可编译桩**（`sha256_hex` 返回空串、`auth::resolve` 返回 false）、CMake 测试目标；预期 `ctest` 中 `auth` FAIL，但工程能编译。
2. `phase1: 注册/登录/登出与 Bearer 鉴权｜token 只存哈希，登出立即失效`：实现真实逻辑 + 路由 + 冒烟断言 + 计划勾选；预期 `ctest` 全绿。

**接口契约：**

| 方法与路径 | 请求体 / 头 | 成功 | 失败 |
|---|---|---|---|
| `POST /api/register` | `{"username","password"}` | 201 `{"user_id","token","expires_at"}` | 400 `invalid_username` / `invalid_password`；409 `username_taken` |
| `POST /api/login` | `{"username","password"}` | 200 `{"user_id","token","expires_at"}` | 400 `invalid_request`；401 `invalid_credentials` |
| `POST /api/logout` | `Authorization: Bearer <token>` | 204 无响应体 | 401 `unauthorized` |
| `GET /api/files` | `Authorization: Bearer <token>` | 200 见 Task 4 | 401 `unauthorized` |

**校验规则：** 用户名 3–20 个字符，仅 `A-Z a-z 0-9 _ -`；密码 8–64 字节；请求体超过 16 KB 返回 413 `payload_too_large`；缺失字段返回 400。

**步骤：**

- [x] 先写 `server/tests/test_auth.cpp`（配套上面的桩，预期 5 条断言全失败）：

```cpp
#include <cstdio>
#include <ctime>
#include <string>
#include "httplib.h"
#include "db/db.h"
#include "db/users.h"
#include "db/sessions.h"
#include "util/crypto.h"
#include "api/auth.h"

static int g_fail = 0;
static void expect(bool ok, const char* name) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_fail;
}

int main() {
    std::string err;
    expect(db::open(":memory:", err), "open memory db");
    db::init_schema(err);

    long long uid = 0;
    db::create_user("alice", "salt", "hash", 5368709120LL, uid, err);

    const std::string token = crypto::random_hex(32);
    expect(token.size() == 64, "token is 64 hex chars");

    const std::time_t now = std::time(nullptr);
    expect(db::create_session(uid, crypto::sha256_hex(token), now + 3600, err), "create session");

    httplib::Request req;
    req.set_header("Authorization", "Bearer " + token);
    long long got = 0;
    expect(auth::resolve(req, got), "valid token resolves");
    expect(got == uid, "resolved user_id");

    httplib::Request no_header;
    long long ignored = 0;
    expect(!auth::resolve(no_header, ignored), "missing header rejected");

    httplib::Request forged;
    forged.set_header("Authorization", "Bearer " + std::string(64, 'f'));
    expect(!auth::resolve(forged, ignored), "forged token rejected");

    const std::string expired_token = crypto::random_hex(32);
    db::create_session(uid, crypto::sha256_hex(expired_token), now - 1, err);
    httplib::Request expired;
    expired.set_header("Authorization", "Bearer " + expired_token);
    expect(!auth::resolve(expired, ignored), "expired token rejected");

    db::delete_session(crypto::sha256_hex(token), err);
    expect(!auth::resolve(req, ignored), "token rejected after logout");

    db::close();
    std::printf("auth: %s (%d failed)\n", g_fail ? "FAILED" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
```

- [ ] 在 `CMakeLists.txt` 追加（与 `test_schema` 并列）：

```cmake
add_executable(test_auth tests/test_auth.cpp
  src/db/db.cpp src/db/users.cpp src/db/sessions.cpp src/util/crypto.cpp src/api/auth.cpp)
target_include_directories(test_auth PRIVATE src third_party)
target_link_libraries(test_auth PRIVATE SQLite::SQLite3 OpenSSL::Crypto Threads::Threads)
add_test(NAME auth COMMAND test_auth)
```

- [ ] 跑 `ctest`，确认 `auth` 为 Failed（`open memory db` 等前几条可能已 PASS，`valid token resolves` 起应失败），提交第 1 个 commit。

- [ ] 然后追加 `scripts/smoke.sh` 用例（与单元测试互补，验证 HTTP 路线）：

```bash
rg=$(curl -s -o /tmp/wp_reg.json -w '%{http_code}' -X POST "$BASE/api/register" \
  -H 'Content-Type: application/json' \
  -d '{"username":"alice","password":"pass12345"}')
check "register 201" 201 "$rg"
token=$(sed -n 's/.*"token":"\([0-9a-f]*\)".*/\1/p' /tmp/wp_reg.json)
dup=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/api/register" \
  -H 'Content-Type: application/json' -d '{"username":"alice","password":"pass12345"}')
check "duplicate register 409" 409 "$dup"
bad=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/api/login" \
  -H 'Content-Type: application/json' -d '{"username":"alice","password":"wrong1234"}')
check "login wrong password 401" 401 "$bad"
noauth=$(curl -s -o /dev/null -w '%{http_code}' "$BASE/api/files")
check "files without token 401" 401 "$noauth"
```

- [ ] 写 `crypto.h/.cpp`：
  - `pbkdf2_sha256(password, salt, iterations=100000, dklen=32)` → hex；salt 为 32 字节随机数转 hex；
  - `random_hex(byte_len)` 用 `RAND_bytes`；
  - `sha256_hex(bytes)` 供 token 哈希与后续内容校验复用。
  - 注意：PBKDF2 用 `PKCS5_PBKDF2_HMAC`（OpenSSL），迭代次数写成常量便于后续调参。
- [ ] 实现注册：校验 → 查重 → 生成 salt/hash → 插入用户 → 建会话 → 201。
- [ ] 实现登录：按用户名查 → PBKDF2 比对（`CRYPTO_memcmp` 定长比较）→ 建会话 → 200；账号不存在与密码错误**返回同一个 401**，不泄露账号是否存在。
- [ ] 实现鉴权中间件：`auth::resolve(req, out_user_id)` 解析 `Bearer`，查 `sessions` 并校验 `expires_at > now`；失败返回 401 `{"error":"unauthorized"}`。
- [ ] 实现登出：删除当前 token 对应的会话记录 → 204。
- [ ] 编译、重启服务端、跑 `ctest` 与 `scripts/smoke.sh`
  - `ctest` 预期：`schema`、`auth` 两个测试全部 Passed；
  - 冒烟预期：register 201 / duplicate 409 / wrong password 401 全部 PASS。
  - 其中 `files without token 401` 这一条此时仍是 FAIL（`/api/files` 属于 Task 4），Task 4 完成后转绿，属预期。
- [ ] 提交第 2 个 commit（实现 + 冒烟 + 计划勾选），然后 push。

**安全 / 边界说明：** 数据库中只存 salt 与 PBKDF2 结果，不存明文；日志只记录用户名与结果码，不记录密码和 token；登录失败的响应体与耗时不做区分（不存在与密码错误走同一分支）。

---

## Task 4：目录浏览与新建文件夹

**目标：** 登录后可读取任意目录的子项列表与配额使用情况，可新建文件夹；越权访问一律 404。

**文件：**
- Create：`server/src/api/files.h`、`server/src/api/files.cpp`
- Modify：`server/src/main.cpp`（注册路由）
- Modify：`scripts/smoke.sh`（追加目录用例）

**接口契约：**

| 方法与路径 | 请求 | 成功响应 |
|---|---|---|
| `GET /api/files?parent_id=<id>` | `parent_id` 缺省为 0（根目录） | 200 `{"parent_id":N,"items":[{"id","name","kind","size","updated_at"}],"used_bytes":U,"quota_bytes":Q}` |
| `POST /api/folders` | `{"parent_id":N,"name":"..."}` | 201 `{"id":N,"name":"最终名称"}` |

**规则：** 排序为文件夹在前、同类按名称升序（UTF-8 字节序）；`parent_id` 必须是当前用户名下的文件夹，否则 404 `not_found`；**同名不报错，按 PRD R2 自动追加 ` (1)`、` (2)`…，响应里的 `name` 是最终落地的名称**（客户端必须使用这个名称，不要用自己提交的名称）；名称 1–255 字符，禁止 `/ \ : * ? " < > |`、控制字符以及 `.`、`..`；原名无法再加后缀时返回 400 `invalid_name`。

**步骤：**

- [ ] 在 `scripts/smoke.sh` 追加用例（先失败）：

```bash
create=$(curl -s -o /tmp/wp_mk.json -w '%{http_code}' -X POST "$BASE/api/folders" \
  -H "Authorization: Bearer $token" -H 'Content-Type: application/json' \
  -d '{"parent_id":0,"name":"文档"}')
check "create folder 201" 201 "$create"
dupdir=$(curl -s -o /tmp/wp_mk2.json -w '%{http_code}' -X POST "$BASE/api/folders" \
  -H "Authorization: Bearer $token" -H 'Content-Type: application/json' \
  -d '{"parent_id":0,"name":"文档"}')
check "duplicate folder auto renamed 201" 201 "$dupdir"
check_grep "duplicate folder renamed to 文档 (1)" /tmp/wp_mk2.json '"name":"文档 (1)"'
list=$(curl -s -o /tmp/wp_list.json -w '%{http_code}' "$BASE/api/files?parent_id=0" \
  -H "Authorization: Bearer $token")
check "list root 200" 200 "$list"
check_grep "list contains folder" /tmp/wp_list.json '"name":"文档"'
```

- [ ] 实现 `GET /api/files`：鉴权 → 校验 parent 归属 → 查询子项 → 计算 `used_bytes` 与 `quota_bytes` → 200。
- [ ] 实现 `POST /api/folders`：鉴权 → 校验名称与 parent → 按 R2 生成唯一名称并插入（`kind='folder'`，`size=0`）→ 201 返回 `{"id","name"}`（`name` 为最终名称）。
- [ ] 编译、重启、重跑 `scripts/smoke.sh`
  - 预期：新增 4 个断言全部 PASS。

**安全 / 边界说明：** 所有查询都带 `owner_id` 条件（PRD R0）；`parent_id` 来自客户端但只作为数据库主键使用，不参与任何路径拼接（PRD 结论）。

---

## Task 5：持久化与越权、并发验证

**目标：** 证明重启不丢数据、token 重启后仍有效、并发注册只成功一次、跨账户访问被拒绝。

**文件：**
- Modify：`scripts/smoke.sh`（追加跨账户用例）
- Create：`server/README.md`

**步骤：**

- [ ] 追加跨账户用例：注册 `bob`，用 bob 的 token 访问 alice 的 `parent_id`，预期 404。

```bash
rb=$(curl -s -o /tmp/wp_bob.json -w '%{http_code}' -X POST "$BASE/api/register" \
  -H 'Content-Type: application/json' -d '{"username":"bob","password":"pass12345"}')
check "register bob 201" 201 "$rb"
btoken=$(sed -n 's/.*"token":"\([0-9a-f]*\)".*/\1/p' /tmp/wp_bob.json)
cross=$(curl -s -o /dev/null -w '%{http_code}' \
  "$BASE/api/files?parent_id=$(sed -n 's/.*"id":\([0-9]*\).*/\1/p' /tmp/wp_mk.json)" \
  -H "Authorization: Bearer $btoken")
check "cross account 404" 404 "$cross"
```

- [ ] 重启验证：停掉服务端 → 重新启动（同一 `--data-dir`）→ 用重启前的 alice token 请求 `GET /api/files?parent_id=0`，预期 200 且仍能看到"文档"。
- [ ] 并发注册验证：

```bash
seq 2 | xargs -P2 -I{} curl -s -o /dev/null -w '%{http_code}\n' -X POST "$BASE/api/register" \
  -H 'Content-Type: application/json' -d '{"username":"carol","password":"pass12345"}'
```
  - 预期：两个结果里恰好一个 `201`、一个 `409`（依靠 `users.username` 唯一约束，不靠应用层先查后插）。

- [ ] 写 `server/README.md`：依赖、构建命令、启动参数、冒烟脚本用法、数据目录说明（`wangpan.db` + 后续 `blob/`）。
- [ ] 完整跑一次全量冒烟：

```bash
bash scripts/smoke.sh http://127.0.0.1:18080
```
  - 预期：所有断言行首为 `PASS:`，最后输出 `通过 N 失败 0`，退出码 0。

**安全 / 边界说明：** 并发注册依赖数据库唯一约束，这是唯一能防住竞态的做法；不要改成"先查再插"。

---

## 阶段完成清单

- [ ] 5 个任务全部完成
- [ ] `ctest` 通过（`schema`、`auth` 两个测试）
- [ ] `scripts/smoke.sh` 全绿，退出码 0
- [ ] 重启后数据与 token 仍有效
- [ ] 跨账户访问返回 404，未登录访问返回 401
- [ ] `server/README.md` 可直接照着跑起来
- [ ] 未产生任何客户端代码（界面留到阶段 2）

## 回滚

本阶段无数据迁移、无对外发布，回滚成本为零：

- 删除构建产物 `build-wsl/`；
- 删除测试数据目录 `/tmp/wp-test`；
- 服务端进程直接 `Ctrl+C` 结束。

## 验证命令汇总

```bash
cd /mnt/c/Users/bzp20/Desktop/wangpan
cmake -S server -B build-wsl -DCMAKE_BUILD_TYPE=Debug
cmake --build build-wsl -j
ctest --test-dir build-wsl --output-on-failure
./build-wsl/wangpan_server --port 18080 --data-dir /tmp/wp-test &
bash scripts/smoke.sh http://127.0.0.1:18080
```

若 Windows 侧无法访问 `127.0.0.1:18080`（WSL NAT 提示），改用 `wsl hostname -I` 得到的 IP，冒烟脚本第一个参数换成该地址即可，代码不用改。

## 视觉验证

不适用：本阶段没有界面。界面验证在阶段 2 进行，验收尺寸为 1024×640 与 1366×768。
