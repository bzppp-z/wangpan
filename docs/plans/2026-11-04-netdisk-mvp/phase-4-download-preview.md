# 阶段 4：下载（断点续传）与在线预览

**目标：** 文件能下到本地，断了能接着下，下完内容与服务器一致；图片与文本可在主界面右侧预览面板直接查看，不落盘。
**前置条件：** 阶段 3 完成（已有真实文件条目与 blob）。
**产出：** `/api/files/{id}/content`、`/api/files/{id}/preview`、客户端 `DownloadTask`、`PreviewPanel`、`server/tests/test_range.cpp`。

**本阶段界面决策：** Q4 已确认"右侧预览面板"。面板的具体宽度、标题栏、缩放交互、空态/错误态需要在本阶段 UI 开工前用 HTML 草图确认，确认后再写 Qt 代码。配色沿用阶段 2 的 `theme.h` 常量。

**范围边界：** 不做文件夹打包下载、不做批量下载（PRD FR-09 明确不支持）、不做 PDF 预览（FR-11）。传输队列/多任务并发属于阶段 5，本阶段下载与上传各自单任务。

---

## 接口契约

| 方法与路径 | 请求 | 成功 | 失败 |
|---|---|---|---|
| `GET /api/files/{id}/content` | `Authorization`；可选 `Range: bytes=<start>-` 或 `bytes=<start>-<end>` | 200（整文件）或 206（Range）；头含 `Accept-Ranges: bytes`、`Content-Length`、`ETag: "<sha256>"`、`X-Content-SHA256`、`Content-Disposition: attachment; filename*=UTF-8''<转义名>` | 401；404 `not_found`（不存在 / 不属于该账户 / 在回收站）；400 `not_a_file`；416 `range_not_satisfiable`（带 `Content-Range: bytes */<size>`） |
| `GET /api/files/{id}/preview` | `Authorization` | 文本：200 `text/plain; charset=utf-8`，超过 1 MiB 截断并带 `X-Truncated: 1`；图片：200 `image/png` 等对应 MIME | 401；404；400 `not_a_file`；415 `preview_unsupported` |

**Range 规则：** 只接受单区间；`bytes=start-` 到文件末尾，`bytes=start-end` 为闭区间（`end` 越界按末尾截断）；`start >= size` 返回 416。响应必须带 `Content-Range: bytes <start>-<end>/<size>`。

**预览白名单（按扩展名，小写）：**

| 类型 | 扩展名 | 响应 |
|---|---|---|
| 图片 | `png jpg jpeg gif bmp webp` | 原字节 + 对应 MIME |
| 文本 | `txt md log json xml csv cpp h` | 最多 1 MiB + `text/plain` |
| 其他 | 含 `svg`、`pdf`、`docx`、`mp4` 等 | 415，不返回任何内容 |

**预览安全：** 白名单之外一律 415；`svg` 明确排除（可执行脚本，不能在客户端当图片渲染）；响应加 `X-Content-Type-Options: nosniff`。

---

## Task 1：下载接口与 Range 支持

**目标：** 支持整文件下载与单区间续传，越权/回收站中的文件一律 404。

**文件：**
- Create：`server/src/api/download.h`、`download.cpp`
- Create：`server/src/util/range.h`（纯函数：解析 `Range` 头）
- Create：`server/tests/test_range.cpp`
- Modify：`server/src/db/blobs.h`、`blobs.cpp`（按 id 取 blob 的 sha/size）
- Modify：`server/src/main.cpp`（注册路由）
- Modify：`server/CMakeLists.txt`（新源文件、`test_range` 目标）
- Modify：`scripts/smoke.sh`

**步骤：**

- [ ] 先写 `server/tests/test_range.cpp`（此时编译失败）：
  - `parse_range("bytes=0-", 100)` → `{valid=true,start=0,last=99}`；
  - `parse_range("bytes=40-59", 100)` → `{40,59}`；
  - `parse_range("bytes=90-200", 100)` → `{90,99}`（越界截断）；
  - `parse_range("bytes=100-", 100)` → `valid=false`（起点等于大小）；
  - `parse_range("bytes=-10", 100)` → `{90,99}`（后缀形式）；
  - 缺头 / 乱格式 → `valid=false`。
- [ ] 实现 `range.h`（不做 I/O，纯解析），跑 `ctest`，预期 `range` 通过。
- [ ] 实现 `GET /api/files/{id}/content`：
  - 鉴权 → 查 `files`（`owner_id = 当前用户 AND deleted_at IS NULL AND kind='file'`）→ 查 blob；
  - 设置 `Accept-Ranges`、`ETag`、`X-Content-SHA256`、`Content-Disposition`（文件名按 RFC 5987 百分号转义，含中文/空格/括号）；
  - 用 `httplib::Response::set_content_provider(len, "application/octet-stream", ...)`，provider 里 `ifstream` 从 `start + offset` 读 `length` 字节写入 `sink`，不要把整个文件读进内存；
  - Range 合法 → 206 + `Content-Range`；非法 → 416 + `Content-Range: bytes */<size>`。
- [ ] `scripts/smoke.sh` 追加（沿用阶段 3 上传得到的文件 id）：

```bash
fid=$(sed -n 's/.*"file_id":\([0-9]*\).*/\1/p' /tmp/wp_done.json)
full=$(curl -s -o /tmp/wp_dl_full.bin -w '%{http_code}' "$BASE/api/files/$fid/content" \
  -H "Authorization: Bearer $token")
check "download 200" 200 "$full"
check "download size" 5242880 "$(stat -c %s /tmp/wp_dl_full.bin)"
part=$(curl -s -o /tmp/wp_dl_part.bin -D /tmp/wp_dl_hdr.txt -w '%{http_code}' \
  -H "Authorization: Bearer $token" -H 'Range: bytes=1048576-' "$BASE/api/files/$fid/content")
check "range 206" 206 "$part"
check_grep "content-range" /tmp/wp_dl_hdr.txt 'Content-Range: bytes 1048576-5242879/5242880'
check "range body size" 4194304 "$(stat -c %s /tmp/wp_dl_part.bin)"
check "bad range 416" 416 "$(curl -s -o /dev/null -w '%{http_code}' \
  -H "Authorization: Bearer $token" -H 'Range: bytes=5242880-' "$BASE/api/files/$fid/content")"
```

- [ ] 用 bob 的 token 请求 alice 的 `fid` → 404；把文件删除后再请求 → 404（删除接口在阶段 5，本任务用 `sqlite3` 临时置 `deleted_at` 验证即可，验证后恢复）。

**安全 / 边界说明：** 下载响应不包含任何服务器路径；文件名来自数据库，经过转义后进响应头，不参与磁盘路径。

---

## Task 2：预览接口（图片 / 文本，1 MiB 截断）

**目标：** 白名单类型可预览；非白名单明确拒绝；文本超限截断；全程流式，不把大文件读进服务端内存。

**文件：**
- Create：`server/src/api/preview.h`、`preview.cpp`
- Modify：`server/src/util/range.h` 不改；新增 `server/src/util/mime.h`（扩展名 → MIME，供 preview 与 download 复用）
- Modify：`server/src/main.cpp`、`server/CMakeLists.txt`
- Modify：`scripts/smoke.sh`

**步骤：**

- [ ] 写 `util/mime.h`：`const char* mime_for_extension(const std::string& lower_ext)`，返回 `nullptr` 表示不支持。
- [ ] 实现 `GET /api/files/{id}/preview`：
  - 归属与 `deleted_at IS NULL` 校验（同下载）；
  - 取扩展名（最后一个 `.` 之后，转小写）→ 白名单判定；不支持 → 415 `preview_unsupported`；
  - 文本：`length = min(size, 1 MiB)`，`X-Truncated: 1`（当 size > 1 MiB）；`set_content_provider(length, "text/plain; charset=utf-8", ...)`；
  - 图片：`set_content_provider(size, mime, ...)`；
  - 两种都带 `X-Content-Type-Options: nosniff`、`X-Content-SHA256`。
- [ ] 追加冒烟断言：

```bash
# 2 MiB 文本：应截断到 1 MiB
dd if=/dev/urandom of=/tmp/wp_text.bin bs=1M count=2 status=none
# 用上传流程把它传成 big.txt（沿用阶段 3 的 init/chunk/complete 步骤）
prev=$(curl -s -o /tmp/wp_prev.bin -D /tmp/wp_prev_hdr.txt -w '%{http_code}' \
  -H "Authorization: Bearer $token" "$BASE/api/files/$text_fid/preview")
check "text preview 200" 200 "$prev"
check "text preview truncated size" 1048576 "$(stat -c %s /tmp/wp_prev.bin)"
check_grep "truncated header" /tmp/wp_prev_hdr.txt 'X-Truncated: 1'
check "unsupported pdf 415" 415 "$(curl -s -o /dev/null -w '%{http_code}' \
  -H "Authorization: Bearer $token" "$BASE/api/files/$pdf_fid/preview")"
```

- [ ] 图片预览：用 1×1 PNG（`printf 'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==' | base64 -d > /tmp/one.png`）上传后预览，断言 `Content-Type: image/png` 且字节数等于原文件。

**安全 / 边界说明：** 预览不落临时文件（PRD R5）；文本按字节截断，可能出现半个 UTF-8 字符，客户端按原样显示即可，不做二次解码猜测。

---

## Task 3：客户端下载任务（保存位置、.part、续传、校验）

**目标：** 选中文件即可下载；暂停保留已下部分；继续用 Range 补；完成后本地文件可打开且 sha 一致。

**文件：**
- Create：`client/src/transfer/downloadtask.h`、`downloadtask.cpp`
- Modify：`client/src/transfer/transferutil.h`、`transferutil.cpp`（新增 `uniqueLocalPath(dir, name)`：重名追加 ` (1)`；`formatEta`）
- Modify：`client/tests/test_transfer.cpp`（追加两个纯函数用例）
- Modify：`client/src/apiclient.h`、`apiclient.cpp`（新增 `QNetworkRequest requestForDownload(qlonglong fileId, qint64 offset) const`，设置 `Range` 与 `Accept-Encoding: identity`）
- Modify：`client/src/ui/mainwindow.*`（工具栏"下载"按钮、右键菜单、保存对话框、状态栏下载摘要）
- Modify：`client/wangpan.pro`

**步骤：**

- [ ] 追加测试用例（先失败）：

```cpp
void uniqueLocalPathAvoidsOverwrite() { /* 目录已有 a.txt → a (1).txt → a (2).txt */ }
void formatEtaText() { /* 90 秒 → "01:30"；3600 秒 → "1:00:00" */ }
```

- [ ] 实现 `DownloadTask`：
  - 字段：`remoteId`、`remoteName`、`size`、`savePath`、`partPath = savePath + ".part"`、`offset`、状态、速度；
  - 打开保存对话框前置校验：`QStorageInfo(dir).bytesAvailable() < size - offset` → 提示"磁盘空间不足"；
  - `start()`：若 `.part` 存在 → `offset = 文件大小` 并带 `Range: bytes=<offset>-`；服务端返回 200（说明不支持 Range）→ `truncate(0)` 重下；206 → 追加写；
  - 响应 `X-Content-SHA256` 记录下来；写完后用 `HashWorker` 校验，不一致 → 删除 `.part`、状态 `Failed("文件校验失败，请重试")`；
  - 校验通过 → `QFile::rename(.part → savePath)`（同名已存在时用 `uniqueLocalPath` 重新取目标）；
  - `pause()` 关闭文件句柄保留 `.part`；`resume()` 从当前大小续；`cancel()` 关闭并删除 `.part`；
  - 网络中断 → 3 次重试后 `Paused("网络中断，已暂停")`。
- [ ] 主窗口：工具栏"下载"（未选中或选中文件夹时禁用）、右键"下载"；保存对话框默认目录取 `QSettings` 的 `download/lastDir`，首次用 `QStandardPaths::DownloadLocation`；下载完成后写回 `lastDir`。
- [ ] 状态栏加下载摘要：`下载中 · 作业.zip 45% · 2.1 MB/s` + 120×12 进度条，空闲隐藏；与阶段 3 的上传摘要并排，窗口窄时左标签 `elided`，不挤压右侧配额。
- [ ] 手工验证：下 5 MiB → 文件可打开、sha 一致；下到 50% 暂停 → `.part` 存在且大小等于已下字节 → 继续 → 完成；取消 → `.part` 被删除。
- [ ] 大文件续传：1 GB 文件下到 20% 暂停，关闭客户端，重开重新下载同一文件 → 提示发现未完成文件，从已有 `.part` 续传（本阶段以"重新选择保存路径时自动识别同名 `.part`"实现，正式跨会话续传在阶段 5 的传输面板里完成）。

**安全说明：** 本地保存路径完全由用户选择；远程文件名只用于建议默认文件名，经过 `uniqueLocalPath` 处理，不直接拼路径。

---

## Task 4：右侧预览面板与布局适配

**目标：** 双击文件在右侧面板查看图片/文本；不支持的格式给出明确提示；面板出现后主列表不被挤坏。

**文件：**
- Create：`client/src/ui/previewpanel.h`、`previewpanel.cpp`
- Modify：`client/src/ui/theme.h`（新增 `kPanelBg "#FAFBFC"`、`kPanelBorder "#E5E6EB"`）
- Modify：`client/src/ui/mainwindow.h`、`mainwindow.cpp`（第三个 splitter 子控件、双击文件预览、右键"预览"）
- Modify：`client/wangpan.pro`

**界面契约（本任务开工前用草图确认）：**

| 项目 | 规格 |
|---|---|
| 位置与尺寸 | 主 splitter 第三个子控件，默认宽 360，可拖动；最小 280；关闭后列表占满 |
| 标题栏 | 高 40，左侧文件名（12px `#1F2329`，过长省略）+ 右侧"×"关闭按钮（24×24）；下方灰色 12px 显示大小与类型 |
| 文本态 | `QPlainTextEdit` 只读，等宽字体 `Consolas` 12px，不换行折行（`NoWrap`），背景 `#FFFFFF`；截断时标题下加一行 `#86909C` "文件过大，仅显示前 1 MB" |
| 图片态 | 适应面板显示；滚轮以鼠标为中心缩放 10%–500%；双击图片恢复"适应面板"；缩放比例显示在标题右侧 |
| 加载态 | 居中文字"加载中…"，不显示假进度（网络请求本身无进度时） |
| 不支持态 | 居中图标区（纯文字）+ "暂不支持预览，请下载后查看" + 次要按钮"下载" |
| 错误态 | 居中错误文案 + "重试"按钮；404 显示"文件已被删除"并自动刷新列表 |
| 空态 | 面板未选中文件时显示"双击文件可预览" |

**步骤：**

- [ ] 搭建 `QStackedWidget`：空 / 加载 / 文本 / 图片 / 不支持 / 错误，六页，切换只改页，不重建控件。
- [ ] 预览请求走 `ApiClient::requestFor("/api/files/{id}/preview")`；从 `Content-Type` 判断走文本还是图片页；读 `X-Truncated` 头。
- [ ] 图片：`QPixmap::loadFromData`；失败归入"不支持"页（避免半张图）；`resizeEvent` 里按当前缩放模式重算；滚轮事件调 `scale`。
- [ ] 文本：`setPlainText(QString::fromUtf8(data))`；超过 1 MiB 由服务端截断，客户端只负责显示提示。
- [ ] 主窗口：`QSplitter` 依次为 `tree(240) | list(stretch) | preview(360)`；`setStretchFactor(1, 1)`；预览面板 `setVisible(false)` 直到首次打开；双击文件夹仍进目录，双击文件 → 预览。
- [ ] 窗口 1024×640 时：树 200、列表 ≥ 320、预览 300，三者无遮挡；1366×768 时预览 360。
- [ ] 依次验证并截图：图片预览、文本预览（含截断提示）、PDF 415 提示、删掉文件后预览 404 提示。

**视觉验证：** 截图 `docs/ui-shots/phase4-preview-image.png`、`phase4-preview-text.png`、`phase4-preview-unsupported.png`。

---

## 阶段完成清单

- [ ] 4 个任务全部完成
- [ ] `ctest` 中 `range` 通过；`test_transfer` 6 条用例全过
- [ ] 整文件下载 200、Range 206、越界 416 全部符合契约
- [ ] 下载暂停/继续/取消行为正确，完成后 sha 一致
- [ ] 图片、文本可预览；文本 >1 MiB 截断提示；PDF 等 415 提示
- [ ] 预览不产生任何本地临时文件
- [ ] 1024×640 下三栏无遮挡
- [ ] 截图归档 3 张

## 回滚

- 服务端：移除 `api/download.*`、`api/preview.*`、`util/range.h`、`util/mime.h` 与路由，其余阶段不受影响。
- 客户端：移除 `downloadtask.*`、`previewpanel.*`，把主窗口 splitter 恢复为两栏，工具栏"下载"按钮删除。
- 数据：下载与预览都是只读路径，不产生任何服务端状态变更，回滚无数据风险。

## 验证命令汇总

```powershell
$env:PATH = "D:\20250519\QT\Qt5.12.11\5.12.11\mingw73_64\bin;D:\20250519\QT\Qt5.12.11\Tools\mingw730_64\bin;" + $env:PATH

# 服务端测试
wsl -d Ubuntu -- bash -lc "cd /mnt/c/Users/bzp20/Desktop/wangpan && cmake --build build-wsl -j && ctest --test-dir build-wsl --output-on-failure"

# 冒烟
wsl -d Ubuntu -- bash -lc "cd /mnt/c/Users/bzp20/Desktop/wangpan && bash scripts/smoke.sh http://127.0.0.1:18080"

# 客户端
cd c:\Users\bzp20\Desktop\wangpan\client\tests
qmake test_transfer.pro CONFIG+=release; mingw32-make -j4; .\release\test_transfer.exe
cd ..
qmake wangpan.pro CONFIG+=release; mingw32-make -j4; .\release\wangpan.exe
```
