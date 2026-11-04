# 阶段 5：文件管理、回收站与传输队列

**目标：** 重命名、删除到回收站、恢复、彻底删除、清空回收站全部可用且配额随之变化；客户端提供底部传输摘要 + 可展开传输列表，支持暂停/继续/取消/重试，未完成任务重启后可继续。
**前置条件：** 阶段 3（上传）与阶段 4（下载、预览）完成。
**产出：** `/api/trash/*`、`PATCH /api/files/{id}`、`server/tests/test_trash.cpp`、客户端 `TransferManager` / `TransferPanel` / `TrashDialog`。

**本阶段界面决策：** Q3 已确认"底部传输摘要 + 可展开传输列表"，回收站入口未定。两个界面的具体布局需要在本阶段 UI 开工前用 HTML 草图确认，确认后再写 Qt 代码。配色沿用 `theme.h`（新增 `kSuccess #00B42A`、`kDanger #F53F3F` 仅在传输状态与回收站警示中使用）。

**范围边界：** 目录间移动不在本期（PRD 3.2 明确）；回收站还原到原目录；不做分享、不做版本历史。

---

## 接口契约

| 方法与路径 | 请求 | 成功 | 失败 |
|---|---|---|---|
| `PATCH /api/files/{id}` | JSON `{"name":"新名称"}` | 200 `{"id":N,"name":"最终名称"}` | 400 `invalid_name`；404 `not_found`（不存在 / 越权 / 已在回收站） |
| `DELETE /api/files/{id}` | 无 body | 204（连同子项软删除） | 404 `not_found` |
| `GET /api/trash` | 无 body | 200 `{"items":[{"id","name","kind","size","deleted_at","parent_id","parent_name","expires_at"}]}`，只列"回收站里的子树根" | 401 |
| `POST /api/trash/{id}/restore` | 无 body | 200 `{"id","name","parent_id"}`（`name` 为最终名称，冲突按 R2 改名） | 404 `not_found`（不在回收站 / 越权 / 不是子树根） |
| `DELETE /api/trash/{id}` | 无 body | 204（彻底删除子树，blob 引用计数 -1） | 404 `not_found` |
| `DELETE /api/trash` | 无 body | 204（清空当前账户回收站） | 401 |

**规则：**
- 删除 = 软删除（`deleted_at = now`），不扣配额，回收站中的条目仍计入 `used_bytes`（R9）。
- 只有"子树根"出现在回收站列表并可被恢复/彻底删除：即 `deleted_at IS NOT NULL` 且（`parent_id = 0` 或父节点未删除）。
- 恢复时若原父目录已不存在或已删除 → 恢复到根目录；重名按 R2。
- 彻底删除后才减少配额；blob 的 `refcount` 减 1，减到 0 时删除 `blobs` 行与磁盘文件（R1：不能影响其他账户/其他条目）。
- 30 天（R3）：服务端启动时清理一次，之后每 6 小时清理一次；清理等价于对过期子树根执行彻底删除。
- 重命名只允许单个条目，遵循 R7 与 R2；名称未变化不发请求。

---

## Task 1：重命名

**目标：** 单条目重命名；冲突自动改名；越权/回收站中的条目 404。

**文件：**
- Modify：`server/src/db/files.h`、`files.cpp`（`rename_file(owner, id, desired_name, out_name, err)`）
- Create：`server/src/api/manage.h`、`manage.cpp`
- Modify：`server/src/main.cpp`
- Modify：`scripts/smoke.sh`
- Create：`server/tests/test_trash.cpp`（先只写重命名部分，后续任务继续追加）

**步骤：**

- [ ] `test_trash.cpp` 先写：创建用户 → 新建 `a.txt` → `rename_file` 为 `b.txt` 成功 → 再建一个 `a.txt` → 把 `b.txt` 改名为 `a.txt`，预期得到 `a (1).txt`；对已软删除的条目改名返回失败（此时软删除还没实现，先跳过这条，Task 2 补）。
- [ ] 实现 `PATCH /api/files/{id}`：鉴权 → 查询归属且 `deleted_at IS NULL` → 校验名称 → 同父目录下按 R2 取唯一名 → 事务内 `UPDATE files SET name=?, updated_at=?` → 200 返回最终名称。
- [ ] 冒烟追加：

```bash
rn=$(curl -s -o /tmp/wp_rn.json -w '%{http_code}' -X PATCH "$BASE/api/files/$fid" \
  -H "Authorization: Bearer $token" -H 'Content-Type: application/json' \
  -d '{"name":"renamed.bin"}')
check "rename 200" 200 "$rn"
check_grep "rename result" /tmp/wp_rn.json '"name":"renamed.bin"'
```

- [ ] 编译跑 `ctest`，`trash` 测试中重命名部分通过（此时整个测试文件尚未完整，允许后续任务继续补断言）。

**安全说明：** 只改数据库名称，磁盘路径与 blob 完全不动；越权统一 404。

---

## Task 2：软删除、回收站、恢复、彻底删除、清空、30 天清理

**目标：** 删除可恢复；彻底删除释放配额与 blob；过期条目自动清理。

**文件：**
- Modify：`server/src/db/files.*`（`soft_delete_subtree`、`list_trash`、`restore_subtree`、`purge_subtree`、`purge_expired`）
- Modify：`server/src/db/blobs.*`（`decref_and_collect`、`remove_zero_ref`）
- Create：`server/src/api/trash.h`、`trash.cpp`
- Modify：`server/src/main.cpp`（注册路由 + 启动清理 + 后台清理线程）
- Modify：`server/tests/test_trash.cpp`（补齐删除/恢复/彻底删除/过期）
- Modify：`scripts/smoke.sh`
- Modify：`server/src/db/db.cpp`（若阶段 1 未加互斥，这里补上全局 `std::mutex` 包住所有 prepare/step；后台线程与请求线程共用同一连接）

**步骤：**

- [ ] 补齐 `test_trash.cpp`：
  - 建文件夹 `A`，在 `A` 内建文件 `f`（关联 blob，refcount=1）；
  - `soft_delete_subtree(A)`：`A` 与 `f` 的 `deleted_at` 均非空；`used_bytes` 不变；
  - `list_trash` 只返回 `A`（不返回 `f`）；
  - `restore_subtree(A)`：两者恢复；若同时存在新的 `A`，最终名为 `A (1)`；
  - `purge_subtree(A)`：`files` 中两行删除，blob `refcount` 变 0，`blobs` 行删除；
  - 把 `deleted_at` 改成 31 天前，`purge_expired(now)` 后该子树消失。
- [ ] 实现递归 SQL（SQLite 支持 `WITH RECURSIVE`）。递归时**不过滤 `deleted_at`**，把子树里"先前已被单独删除的子孙"也统一成同一个删除时间——语义固定为"删除/恢复整个文件夹时，连同其中的所有内容一起处理"，不会出现父目录恢复了、里面的子项却永远躺在回收站里的半截状态：

```sql
-- 软删除子树（统一时间戳）
WITH RECURSIVE sub(id) AS (
  SELECT id FROM files WHERE id = ? AND owner_id = ?
  UNION ALL
  SELECT f.id FROM files f JOIN sub s ON f.parent_id = s.id
  WHERE f.owner_id = ?
)
UPDATE files SET deleted_at = ? WHERE id IN (SELECT id FROM sub);
```

- [ ] `restore_subtree` 同理：把子树内所有行（含子孙）的 `deleted_at` 置空。

- [ ] 实现 `GET /api/trash`：只取子树根（父节点未删除或 `parent_id=0`），带 `parent_name`（父节点即使已删除也能显示原名称）、`expires_at = deleted_at + 30d`，按 `deleted_at DESC`。
- [ ] 实现 `POST /api/trash/{id}/restore`：校验是子树根 → 父目录若已删除/不存在则 `parent_id = 0` → 按 R2 取唯一名 → 子树 `deleted_at = NULL` → 200。
- [ ] 实现 `DELETE /api/trash/{id}` 与 `DELETE /api/trash`：
  1. 收集子树内所有 `blob_id`；
  2. 事务内删除 `files` 行、对每个 blob `refcount = MAX(refcount - 1, 0)`、删除 `refcount = 0` 的 `blobs` 行并记下 sha；
  3. 提交后按 `blob_path(sha)` 删除磁盘文件；单个文件删除失败只记 warning，不返回失败。
- [ ] 启动时调用一次 `purge_expired(now)`；`main.cpp` 里再起一个 detached 线程，每 6 小时调用一次（进程退出即结束，不 join）。
- [ ] 启动清理顺带处理孤儿 blob：`refcount = 0` 的 `blobs` 行直接删除，并按 `blob_path` 删除磁盘文件（覆盖阶段 3 崩溃时可能留下的"有行无文件/有文件无行"中的第一种；有文件无行的情况会在下次上传同内容时被 `commit_blob` 复用）。
- [ ] 冒烟断言：

```bash
del=$(curl -s -o /dev/null -w '%{http_code}' -X DELETE "$BASE/api/files/$fid" \
  -H "Authorization: Bearer $token")
check "soft delete 204" 204 "$del"
trash=$(curl -s "$BASE/api/trash" -H "Authorization: Bearer $token")
check_grep "trash contains renamed" <<<"$trash" '"name":"renamed.bin"'
check "restore 200" 200 "$(curl -s -o /dev/null -w '%{http_code}' -X POST \
  "$BASE/api/trash/$fid/restore" -H "Authorization: Bearer $token")"
check "delete again 204" 204 "$(curl -s -o /dev/null -w '%{http_code}' -X DELETE \
  "$BASE/api/files/$fid" -H "Authorization: Bearer $token")"
check "purge 204" 204 "$(curl -s -o /dev/null -w '%{http_code}' -X DELETE \
  "$BASE/api/trash/$fid" -H "Authorization: Bearer $token")"
check "trash empty" 0 "$(curl -s "$BASE/api/trash" -H "Authorization: Bearer $token" | grep -c '"id"')"
```

- [ ] 配额验证：彻底删除后用 `GET /api/files` 检查 `used_bytes` 减少恰好一个文件大小；仅软删除时不减少。

**安全 / 边界说明：** 所有子树操作都带 `owner_id` 条件；彻底删除是唯一会调 `refcount` 的入口，且必须在事务内完成计数变更；磁盘删除放在事务提交后，避免回滚后文件先没了。

---

## Task 3：客户端传输管理器与传输面板（Q3）

**目标：** 上传/下载统一进队列；同时最多 3 个任务；暂停/继续/取消/重试/清空可用；未完成任务重启后可继续。

**文件：**
- Create：`client/src/transfer/transfermanager.h`、`transfermanager.cpp`
- Create：`client/src/ui/transferpanel.h`、`transferpanel.cpp`
- Modify：`client/src/transfer/uploadtask.*`（暴露 `setApiClient`、`sessionId`、`localPath` 等供序列化）
- Modify：`client/src/transfer/downloadtask.*`（同上）
- Modify：`client/src/ui/mainwindow.*`（用 manager 替换阶段 3/4 的单任务逻辑；底部摘要点击展开）
- Modify：`client/wangpan.pro`

**界面契约（开工前用草图确认）：**

| 项目 | 规格 |
|---|---|
| 收起态 | 窗口底部一行 32 高：左侧"传输：2 进行中 · 1 等待 · 3.4 MB/s"，右侧 ↑↓ 箭头；点击展开/收起；无任务时整行隐藏 |
| 展开态 | 高 180，位于文件列表下方、状态栏上方，可拖动分隔条调整（140–320） |
| 列表列 | 名称（拉伸，省略号）｜方向（64）｜大小（90）｜进度条（120，条内显示百分比）｜速度（90）｜剩余（80）｜状态（80） |
| 状态颜色 | 进行中 `#2D8CF0`；已完成 `#00B42A`；失败 `#F53F3F`；已暂停/取消 `#86909C`；等待中 `#1F2329` |
| 操作 | 右键菜单：暂停/继续/取消/重试/打开所在位置；顶部右侧"清空已完成"按钮 |
| 空态 | "没有传输任务" 居中 `#86909C` |
| 并发 | 最多 3 个"进行中"；其余显示"等待中"，按创建顺序启动 |

**步骤：**

- [ ] 实现 `TransferManager`：
  - `addUpload(localPath, parentId, desiredName)` / `addDownload(remoteId, remoteName, savePath)`；
  - 队列：`runningCount < 3` 才 `start`，任务结束（任意终态）后从队列取下一个；
  - `pause(id)` / `resume(id)` / `cancel(id)` / `retry(id)` / `clearCompleted()` 转发到具体任务；
  - 每 500 ms 汇总一次速度与 ETA（3 秒滑动窗口），发 `summaryChanged(QString text, bool anyActive)`；
  - 任务状态变化发 `taskUpdated(QString id)`，供面板刷新一行；
  - `UploadTask` / `DownloadTask` 增加稳定 `id`（`QUuid`）。
- [ ] 持久化：`QStandardPaths::AppDataLocation + "/transfers.json"`，格式：

```json
{"version":1,"items":[
  {"type":"upload","localPath":"C:/x/a.zip","parentId":0,"name":"a.zip","size":123,"sha256":"..."},
  {"type":"download","remoteId":7,"name":"a.zip","savePath":"C:/Users/bzp20/Downloads/a.zip","size":123}
]}
```

  - 登录成功后恢复：上传任务若本地文件不存在 → 标记失败"源文件不存在"；下载任务若 `.part` 存在则按已下字节续传；
  - 只在"未完成"任务（Waiting/Running/Paused/Failed）写入文件；Completed/Canceled 不写；启动恢复后不自动开始，状态置"等待中"，用户点继续。
- [ ] 实现 `TransferPanel`：`QTableWidget` 8 列 + 右键菜单 + 顶部"清空已完成"；收起态与状态栏摘要联动（摘要点击展开）。
- [ ] 主窗口替换：阶段 3 的 `UploadController` 与阶段 4 的下载入口统一改调 `TransferManager`；工具栏"上传"菜单、"下载"按钮、拖拽都进队列。
- [ ] 手工验证：
  - 同时拖入 4 个文件 → 3 个进行中、1 个等待；某个完成后等待的自动开始；
  - 暂停 1 个 → 其余不受影响；继续 → 从原进度走；
  - 关掉客户端（任务未完成）→ 重开登录 → 传输行仍在，状态"等待中"，点继续可完成。

**视觉验证：** 截图 `docs/ui-shots/phase5-transfer-collapsed.png`、`phase5-transfer-expanded.png`。

---

## Task 4：文件操作与回收站界面

**目标：** 右键/F2/Delete 完成重命名与删除；回收站可恢复、彻底删除、清空；操作后列表与配额即时更新。

**文件：**
- Create：`client/src/ui/trashdialog.h`、`trashdialog.cpp`
- Modify：`client/src/apiclient.*`（新增 `renameFile`、`deleteFile`、`listTrash`、`restoreTrash`、`purgeTrash`、`clearTrash` 及对应信号）
- Modify：`client/src/ui/mainwindow.*`
- Modify：`client/wangpan.pro`

**界面契约（开工前用草图确认）：**

| 项目 | 规格 |
|---|---|
| 入口 | 工具栏"回收站"按钮（尺寸与其它工具栏按钮一致），或右键菜单"回收站" |
| 窗口 | 720×480，模态；标题"回收站"；右上"清空回收站"（文字按钮，`#F53F3F`） |
| 列表列 | 名称（拉伸）｜大小（100）｜删除时间（160，`yyyy-MM-dd HH:mm`）｜剩余天数（90，`X 天后彻底删除`） |
| 行操作 | 双击或多选后：底部"恢复"、"彻底删除"按钮 |
| 确认 | 彻底删除 / 清空 → `QMessageBox::warning`，文案"彻底删除后不可恢复，确定继续？"；软删除不做确认 |
| 空态 | "回收站是空的" |

**步骤：**

- [ ] 右键菜单固定顺序：预览、下载、重命名、删除、回收站；文件夹行隐藏"预览"与"下载"。
- [ ] `F2` 仅单选可用；`Delete` 支持多选，逐个调用删除接口后刷新当前目录与配额。
- [ ] 重命名对话框默认填入当前名称并全选；名称为空或未变化 → 不请求（未变化直接关闭）。
- [ ] 删除后若当前目录变空 → 显示阶段 2 的空态文案；若删除的是当前所在文件夹的祖先，回根目录。
- [ ] `TrashDialog`：打开时拉取列表；恢复后刷新回收站与主窗口；彻底删除/清空需确认；全部为空时禁用两个按钮。
- [ ] 错误处理：404 → "文件已被删除"并刷新；401 → 回登录窗。
- [ ] 手工验证：建 `文档/A` → 重命名为 `B` → 删除 `B` → 回收站出现 `B` → 恢复 → 主列表出现 `B` → 再次删除 → 彻底删除 → 配额减少。

---

## Task 5：整体验收（配额、跨账户、恢复冲突、并发、持久化）

**目标：** 把本阶段的行为在真实服务器上一次性走通。

**文件：**
- Modify：`scripts/smoke.sh`（追加跨账户回收站/重命名 404 断言）
- Modify：`server/README.md`（补回收站与清理线程说明）

**步骤：**

- [ ] 配额账本：上传 100 MB → 配额 +100；删除（软）→ 不变；彻底删除 → 恢复原值；清空回收站 → 与操作前一致。
- [ ] 跨账户：bob 对 alice 的文件 `PATCH` / `DELETE` / `restore` / `purge` 全部 404；bob 的回收站看不到 alice 的条目。
- [ ] 恢复冲突：把 `文档` 删除后在根目录再建 `文档`，恢复 → 得到 `文档 (1)`。
- [ ] 30 天清理：手工把一个条目的 `deleted_at` 改成 31 天前，重启服务端 → 回收站中消失、blob 文件被删除。
- [ ] 并发：4 个任务同时跑，最多 3 个进行中；全部完成后本地/远程 sha 一致。
- [ ] 持久化：暂停 1 个上传与 1 个下载 → 关闭客户端 → 重开登录 → 两条记录都在 → 继续后完成。
- [ ] 布局：1024×640 下展开传输面板，文件列表仍有 ≥ 320 宽；收起后布局恢复。
- [ ] 截图汇总：`docs/ui-shots/` 下本阶段 4 张（收起、展开、回收站、彻底删除确认）。

## 阶段完成清单

- [ ] 5 个任务全部完成
- [ ] `ctest` 中 `trash` 全过；`scripts/smoke.sh` 全绿
- [ ] 软删除不扣配额、彻底删除精准扣减
- [ ] 恢复冲突按 R2 改名；原目录不存在时回到根目录
- [ ] 跨账户所有写操作 404
- [ ] 30 天清理在启动与 6 小时周期内生效
- [ ] 传输最多 3 并发，暂停/继续/取消/重试/清空全部可用
- [ ] 未完成任务重启后可继续
- [ ] 4 张截图归档

## 回滚

- 服务端：移除 `api/manage.*`、`api/trash.*` 与相关路由；`deleted_at` 列保留（阶段 1 已建），不影响其他阶段。
- 客户端：移除 `transfermanager.*`、`transferpanel.*`、`trashdialog.*`，恢复阶段 4 的单任务上传/下载入口。
- 数据：彻底删除不可逆，回滚前先备份 `wangpan.db` 与 `blob/`；软删除数据可通过恢复接口还原。

## 验证命令汇总

```powershell
$env:PATH = "D:\20250519\QT\Qt5.12.11\5.12.11\mingw73_64\bin;D:\20250519\QT\Qt5.12.11\Tools\mingw730_64\bin;" + $env:PATH

wsl -d Ubuntu -- bash -lc "cd /mnt/c/Users/bzp20/Desktop/wangpan && cmake --build build-wsl -j && ctest --test-dir build-wsl --output-on-failure"
wsl -d Ubuntu -- bash -lc "cd /mnt/c/Users/bzp20/Desktop/wangpan && bash scripts/smoke.sh http://127.0.0.1:18080"

cd c:\Users\bzp20\Desktop\wangpan\client\tests
qmake test_transfer.pro CONFIG+=release; mingw32-make -j4; .\release\test_transfer.exe
cd ..
qmake wangpan.pro CONFIG+=release; mingw32-make -j4; .\release\wangpan.exe
```
