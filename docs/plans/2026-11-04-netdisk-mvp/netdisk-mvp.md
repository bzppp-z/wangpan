# 多用户网盘 MVP 实施计划（总索引）

**创建日期：** 2026-11-04
**状态：** In Progress
**需求来源：** `docs/PRD.html`（v0.3，需求已确认）+ 对话中确认的 M1–M4

## 概览

**目标：** 交付一个可部署到阿里云、支持多账户注册登录、具备分片上传 / 断点续传 / 秒传去重 / 下载 / 在线预览 / 回收站 / 配额的 C++ 网盘。

**架构：** 客户端是 Windows 上的 Qt 5.12 Widgets 程序，通过 HTTP + JSON 访问服务端；服务端是 C++14 + cpp-httplib + SQLite 的 Linux 程序，在 WSL2 中编译；文件内容按 SHA-256 内容寻址落盘，元数据存 SQLite；上线时由 systemd 常驻、Nginx 反向代理。

**技术栈：** C++14、cpp-httplib（`server/third_party/httplib.h`）、nlohmann/json（`server/third_party/nlohmann/json.hpp`）、SQLite3、OpenSSL（PBKDF2 + SHA-256）、CMake、Qt 5.12.11 (mingw73_64)、Nginx、systemd。

## 文件与职责（全项目）

| 文件/模块 | 动作 | 职责 | 备注 |
|---|---|---|---|
| `server/CMakeLists.txt` | Create | 服务端构建：C++14、头文件路径、链接 sqlite3/OpenSSL/pthread | 阶段 1 |
| `server/src/main.cpp` | Create | 入口、参数解析、路由注册、启动日志 | 阶段 1 |
| `server/src/config.h` `config.cpp` | Create | 端口、绑定地址、数据目录、分片大小等配置 | 阶段 1 |
| `server/src/log.h` | Create | 分级日志（带时间戳，禁止输出密码/token） | 阶段 1 |
| `server/src/db/schema.h` | Create | 建表 SQL（内嵌 raw string，幂等执行） | 阶段 1 |
| `server/src/db/db.h` `db.cpp` | Create | SQLite 打开/关闭、WAL、事务、错误处理 | 阶段 1 |
| `server/src/db/users.h` `users.cpp` | Create | 用户创建、按名查询、配额已用统计 | 阶段 1 |
| `server/src/db/sessions.h` `sessions.cpp` | Create | 会话创建、按 token 哈希校验、失效 | 阶段 1 |
| `server/src/db/files.h` `files.cpp` | Create | 目录项查询、新建文件夹、重名自动编号（R2）；阶段 5 追加重命名/软删除/恢复/彻底删除/过期清理 | 阶段 1、5 |
| `server/src/db/blobs.h` `blobs.cpp` | Create | blob 查建、引用计数增减、零引用清理 | 阶段 3、5 |
| `server/src/util/crypto.h` `crypto.cpp` | Create | PBKDF2 密码哈希、随机 token、SHA-256 | 阶段 1 |
| `server/src/api/respond.h` | Create | 统一 JSON 响应与错误码 | 阶段 1 |
| `server/src/api/auth.h` `auth.cpp` | Create | 注册、登录、登出、Bearer 鉴权中间件 | 阶段 1 |
| `server/src/api/files.h` `files.cpp` | Create | 目录列表、新建文件夹接口 | 阶段 1 |
| `server/src/storage/blob.h` `blob.cpp` | Create | 分片落盘、扫描、合并、哈希、内容寻址提交 | 阶段 3 |
| `server/src/api/upload.h` `upload.cpp` | Create | 上传 init / 分片 / 状态 / 完成 / 取消 / 秒传 | 阶段 3 |
| `server/src/api/download.h` `download.cpp` | Create | 下载与 Range 支持 | 阶段 4 |
| `server/src/api/preview.h` `preview.cpp` | Create | 图片与文本预览（白名单 + 1 MiB 截断） | 阶段 4 |
| `server/src/api/manage.h` `manage.cpp` | Create | 重命名 | 阶段 5 |
| `server/src/api/trash.h` `trash.cpp` | Create | 软删除、回收站列表、恢复、彻底删除、清空、30 天清理 | 阶段 5 |
| `server/src/util/range.h` `mime.h` | Create | Range 解析、扩展名 → MIME 白名单 | 阶段 4 |
| `server/tests/test_schema.cpp` | Create | 建表与基础读写测试（CTest） | 阶段 1 |
| `server/tests/test_auth.cpp` `test_storage.cpp` `test_instant.cpp` `test_range.cpp` `test_trash.cpp` | Create | 鉴权与会话、存储合并、秒传归属、Range 解析、回收站与引用计数 | 阶段 1、3、4、5 |
| `client/src/transfer/*` | Create | `transferutil`、`hashworker`、`uploadtask`、`downloadtask`、`transfermanager` | 阶段 3、4、5 |
| `client/src/ui/previewpanel.*` `transferpanel.*` `trashdialog.*` `uploadcontroller.*` | Create | 预览面板、传输面板、回收站对话框、上传路径编排 | 阶段 3、4、5 |
| `scripts/smoke.sh` | Create | 端到端冒烟脚本（curl 断言） | 阶段 1 起，逐阶段追加 |
| `client/wangpan.pro` 及 `client/src/*` | Create | Qt 客户端工程与界面 | 阶段 2+ |
| `deploy/wangpan.service` `deploy/nginx.conf` `deploy/install.sh` `deploy/README.md` `scripts/package-client.ps1` | Create | 部署、反向代理、安装脚本与客户端打包 | 阶段 6 |

## 界面实现映射

界面决策（PRD 第 6 节 Q1–Q4）对应实现与验证如下。**阶段 2 的登录窗与主界面草图已于 2026-11-04 确认**（`docs/ui-mockups/phase-2-ui-sketch.html`）；阶段 3/4/5 涉及的局部界面（上传摘要、预览面板、传输面板、回收站）仍需在各阶段 UI 任务开工前用同样的 HTML 草图确认：

| 已确认决策 | 来源 | 实现位置 | 验证方式 |
|---|---|---|---|
| 左目录树 + 右文件列表 | Q1 | 阶段 2 `client/src/ui/mainwindow.*` | 1024×640 与 1366×768 两种窗口尺寸手动核对 |
| 独立登录/注册界面 | Q2 | 阶段 2 `client/src/ui/loginwindow.*` | 手动走登录、注册、错误提示 |
| 底部传输摘要 + 可展开传输列表 | Q3 | 阶段 5 `client/src/ui/transferpanel.*` | 同时 3 个任务时进度与状态正确 |
| 右侧预览面板 | Q4 | 阶段 4 `client/src/ui/previewpanel.*` | 图片/文本预览、列表变窄时无遮挡 |
| 免安装文件夹分发 | M4 | 阶段 6 `scripts/package-client.ps1` | 在无 Qt 环境的机器解压运行 |

## 阶段划分

### 阶段 1：服务端骨架与账户/目录接口
**文件：** [phase-1-server-foundation.md](phase-1-server-foundation.md)
**目标：** 服务端能编译、启动、建库，注册/登录/登出/鉴权/目录列表/新建文件夹全部可用，并通过冒烟脚本验证。
**任务：** 5
**状态：** Done

### 阶段 2：Qt 客户端框架与登录、文件列表
**文件：** [phase-2-qt-client-login.md](phase-2-qt-client-login.md)
**目标：** 客户端能配置服务器地址、登录/注册、展示目录列表并进入子目录；与服务端联调通过。
**任务：** 5
**状态：** Done

### 阶段 3：上传（分片、断点续传、秒传、配额）
**文件：** [phase-3-upload.md](phase-3-upload.md)
**目标：** 任意大小文件可上传，中断后可继续，相同内容秒传，超配额被拒绝，落盘内容与 SHA-256 一致。
**任务：** 6
**状态：** Done

### 阶段 4：下载与在线预览
**文件：** [phase-4-download-preview.md](phase-4-download-preview.md)
**目标：** 下载支持断点续传（Range），图片与文本可在右侧预览面板查看，预览不落盘。
**任务：** 4
**状态：** Done

### 阶段 5：文件管理、回收站与传输队列
**文件：** [phase-5-file-ops-trash-queue.md](phase-5-file-ops-trash-queue.md)
**目标：** 重命名、删除进回收站、恢复、彻底删除、清空回收站，配额随之变化；客户端提供 3 并发传输队列与可展开传输面板。
**任务：** 5
**状态：** Not Started

### 阶段 6：部署到阿里云
**文件：** [phase-6-deploy.md](phase-6-deploy.md)
**目标：** 服务端在 Ubuntu 22.04 上由 systemd 常驻、Nginx 转发、重启后可恢复；客户端打包为免安装文件夹并可用公网地址完成上传下载。
**任务：** 5
**状态：** Not Started

## 依赖与假设

- WSL2 发行版名为 `Ubuntu`（实测 g++ 15.2.0、cmake 4.2.3、`/usr/include/sqlite3.h`、`/usr/include/openssl/sha.h` 均存在）。代码按 Ubuntu 22.04 可编译的标准书写（C++14，不用 gcc 15 才有的特性）。
- 服务端依赖头文件已就位：`server/third_party/httplib.h`（309,946 字节）、`server/third_party/nlohmann/json.hpp`。
- Qt 5.12.11 位于 `D:\20250519\QT\Qt5.12.11\5.12.11\mingw73_64`（qmake 已确认存在，随附 MinGW 7.3）。
- 通信协议固定为 HTTP/1.1 + JSON；本期按 M3 不上 HTTPS。
- 单机单节点部署，SQLite 单文件数据库，文件内容与数据库都放服务端数据目录。
- 存储路径由服务端生成，客户端传入的只有 id 与文件名；文件名只进数据库，不参与磁盘路径拼接。
- 阶段 2 的界面契约与草图已确认；阶段 3/4/5 的局部界面在各自 UI 任务开工前先出 HTML 草图征得确认，确认前不写对应 Qt 代码。

## 成功标准

- [ ] 六个阶段全部完成，PRD 中 FR-00 ~ FR-11 均有对应实现
- [ ] `scripts/smoke.sh` 全绿（覆盖注册、登录、鉴权、目录、上传、断点续传、秒传、下载、预览、回收站）
- [ ] 服务端在 WSL 中零警告编译；重启后数据完整
- [ ] 两个账户互相看不到对方文件（R0 账户隔离）
- [ ] 单文件上传/下载在 1 GB 文件上不断、不损坏（SHA-256 校验一致）
- [ ] 阿里云部署后，另一台机器上的客户端能完成一次上传与下载
- [ ] 安全假设已验证：无明文密码落库、路径不来自客户端、未登录接口全部拒绝

## 简历证据与 Git 流程

本项目的另一个目标是被写进简历并讲清楚，所以"能跑"之外还要留下可验证的过程证据。

### 仓库与提交

- 仓库已初始化并关联 `https://github.com/bzppp-z/wangpan`，`main` 已推送；后续每个任务提交后直接 `git push`。
- 忽略规则见根目录 `.gitignore`：`build-wsl/`、`data/`、`*.db`、`dist/`、`client/**/release`、`*.pro.user`、`Makefile`、`.qmake.stash`、日志与密钥一律不提交。
- **每完成一个计划任务 = 一次本地提交 + 立即推送**。消息格式：`phaseN: 任务名｜关键决策`，例如 `phase3: 分片上传与合并｜服务端自算 SHA-256 防伪造`。
- **总提交数 33**：30 个任务各 1 次提交；其中「Bearer 鉴权」（阶段 1 Task 3）、「分片合并与哈希」（阶段 3 Task 1）、「秒传判定」（阶段 3 Task 2）三个核心模块各拆成 `test(xxx): …｜先红` + `phaseN: …｜转绿` 两次提交，提交内含可编译桩，保证每一步仓库都能编译。
- 推送失败（网络问题）**不阻塞开发**：本地提交照常进行，恢复后一次 `git push`，提交历史仍然完整；不要为了推送把多个任务合并成一个提交。
- 每阶段完成打 tag：`v0.1-phase1`、`v0.2-phase2` … `v1.0`；阶段 6 把免安装 zip 挂到 GitHub Release。
- 已推送的提交不 `amend`、不强推；需要返工就新增一个修复提交。
- 如果某个任务中途发现计划有误，先改计划文件再改代码，计划与代码一起提交。

### 实测数据留存

每个阶段结束后把真实数字填进下表（简历里的数字只能来自这里，不估算、不编造）：

| 阶段 | 要记录的实测值 | 采集方式 |
|---|---|---|
| 1 服务端 | 冒烟断言条数、冷启动到可服务耗时、并发注册结果 | `scripts/smoke.sh` 输出 |
| 2 客户端 | 登录→列表耗时、1024×640 截图 | 秒表 + `docs/ui-shots/` |
| 3 上传 | 1 GB 上传耗时与平均吞吐、秒传耗时、断点续传补传字节数 | 客户端计时 + 服务端日志 |
| 4 下载 | 1 GB 下载耗时、Range 续传起始偏移、预览大文本耗时 | curl `-w` + 客户端 |
| 5 文件管理 | 3 并发任务下总耗时、彻底删除后配额回退值 | 客户端 + `/api/files` |
| 6 部署 | 公网上传/下载吞吐、重启恢复耗时、服务内存峰值 | `deploy/README.md` 记录 |

已采集（2026-11-10，本机 WSL2 + Windows 客户端）：

| 阶段 | 实测值 |
|---|---|
| 3 上传 | 1 GiB 上传 89 s（约 12 MB/s，含客户端本地 SHA-256）；1 GiB 秒传 `init` 17 ms；120 MiB 断点续传从 16,777,216 字节处继续；冒烟 62 条断言全 PASS；`ctest` 4/4 通过 |
| 4 下载 | 1 GiB 下载 117.2 s（9.16 MB/s）；`Range: bytes=214748364-` 续传 820 MiB 142.3 s（6.04 MB/s），`Content-Range` 校验通过；下载后 sha256 与上传一致；上传 1 GiB 178 s（5.9 MB/s，4 MiB 分片）；下载/预览冒烟 79 / 93 条断言全 PASS；`ctest` 5/5、`test_transfer` 6/6 通过 |

### 面试可讲性（开发时就要守住的底线）

- 至少亲手重写并讲清 3 个模块：分片合并与 SHA-256 校验、Bearer 鉴权中间件、秒传判定；其余模块也要能解释设计取舍。
- 代码里不出现"明显不统一"的风格（命名、错误码、日志格式），面试官会直接看仓库。
- 保留每次踩坑的 commit 或简短笔记（例如"半截分片导致 sha 不符"），这是面试里最真实的素材。

## 阻塞问题

- 已解除：阶段 2 登录窗/主窗口草图已于 2026-11-04 确认，阶段 1 与阶段 2 可立即按计划执行。
- 已解除：阶段 3 上传摘要（`docs/ui-mockups/phase-3-upload-status.html`）与阶段 4 预览面板（`docs/ui-mockups/phase-4-preview-panel.html`）草图已在对应 UI 任务前产出并自检。
- 待确认：阶段 5 的传输面板与回收站草图，需在对应 UI 任务开工前产出确认，确认前只做该阶段的服务端部分。

## 延后决策

- **Windows 访问 WSL 服务的连通性**：WSL 启动时有"localhost 代理未镜像"的提示。若 Windows 侧 `curl http://127.0.0.1:8080` 不通，改用 `wsl hostname -I` 得到的 IP 访问；仍不通则在 `.wslconfig` 开启网络镜像。默认按 `127.0.0.1` 编写，不为此改代码。
- **服务端默认端口**：8080（可用 `--port` 覆盖），不与阿里云 Nginx 的 80/443 冲突。
- **单元测试框架**：阶段 1 只用 CTest + 自写断言；后续阶段若要跑分片/合并等复杂逻辑，再引入 Catch2 这类框架，属于扩展而非必需。
- **数据库**：本期固定 SQLite；行访问集中在 `server/src/db/*`，换 MySQL 时只改这一层。
- **备份**：不在 MVP 范围；上线后第一步补每日备份（PRD 已知风险）。
