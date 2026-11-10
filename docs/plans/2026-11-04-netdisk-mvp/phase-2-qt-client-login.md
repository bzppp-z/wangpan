# 阶段 2：Qt 客户端框架与登录 / 文件列表

**目标：** 交付一个能在 Qt Creator 中运行、也能命令行编译的 Windows 客户端：可按百度网盘的登录交互完成登录/注册，登录后展示左目录树 + 右文件列表，可进入子目录、新建文件夹、退出登录。
**前置条件：** 阶段 1 完成（服务端 `/api/health`、`/api/register`、`/api/login`、`/api/logout`、`/api/files`、`/api/folders` 可用）。
**产出：** `client/release/wangpan.exe`、`client/tests/test_apiclient.exe`、界面截图、`client/README.md`。

**本阶段实现的界面决策：** Q1 左目录树 + 右文件列表；Q2 独立登录/注册窗口；登录交互参照百度网盘（账号登录 / 扫码登录双入口、蓝色主按钮、底部注册与设置入口）。**不复制百度网盘的商标、Logo、字体文件与图片资源**，只复用布局结构与交互逻辑。

---

## 界面设计契约（实现前必须先确认，实现后按此验收）

### 登录 / 注册窗口

| 项目 | 规格 |
|---|---|
| 窗口尺寸 | 760 × 440，固定大小，居中显示，无最大化按钮 |
| 左侧品牌区 | 宽 320，背景从 `#F5F8FF` 到 `#EAF2FF` 垂直渐变；文字仅"网盘"（28px 粗体 `#1F2329`）与副标题"私有云存储 · 多用户版"（13px `#86909C`）；不含任何第三方品牌素材 |
| 右侧表单区 | 宽 440，背景 `#FFFFFF`，内容左右内边距 40 |
| 顶部页签 | "账号登录"（选中：文字 `#1F2329`，底部 2px `#2D8CF0` 横线）与"扫码登录"（禁用：文字 `#C9CDD4`，鼠标悬停提示"暂未开放"）；页签高 44 |
| 输入框 | 高 40，宽 360，圆角 4，边框 `#D9D9D9`，聚焦边框 `#2D8CF0`；占位文字分别为"用户名"与"密码" |
| 密码框 | 默认掩码显示；右侧"显示/隐藏"按钮（24×24，图标用 Qt 内置 `QStyle::SP_DialogYesButton` 风格文本"显示"/"隐藏"，不引入 icon 文件） |
| 主按钮 | 高 40、宽 360、圆角 4、背景 `#2D8CF0`、文字白色"登 录"；悬停 `#1E6FD9`；禁用 `#A0CFFF`；按下 `#1A5FB4` |
| 错误条 | 位于按钮上方，高 32，背景 `#FFF1F0`，文字 `#E64545`，圆角 4，默认隐藏 |
| 底部链接行 | 左"注册账号"（`#2D8CF0`），右"设置服务器"（`#86909C`）；点击"设置服务器"弹出单行输入框修改基地址 |
| 注册态 | 同一窗口切换：字段变为"用户名""密码""确认密码"，主按钮文字"注 册"，底部链接变"返回登录"，页签隐藏 |
| 交互状态 | 空闲 / 提交中（按钮文字"登录中…"，输入框与按钮禁用，防止重复提交）/ 失败（错误条显示服务端 message）/ 成功（关闭登录窗，打开主窗口） |
| 键盘 | Enter 提交；Tab 顺序：用户名 → 密码 → 显示密码 → 登录 → 注册账号 → 设置服务器；Esc 关闭窗口并退出程序 |
| 记住内容 | 用 `QSettings("Wangpan", "Client")` 只保存上次用户名与服务器地址；**绝不保存密码** |

### 主窗口

| 项目 | 规格 |
|---|---|
| 窗口尺寸 | 默认 1100 × 700，最小 1024 × 640，可最大化 |
| 顶部工具栏 | 高 48，按钮：新建文件夹、刷新、退出登录；"上传"按钮存在但禁用，悬停提示"阶段 3 开放" |
| 左侧目录树 | 宽 240，节点为文件夹，根节点"我的网盘"；选中后右侧列表随之刷新 |
| 右侧文件列表 | 列：名称（自适应拉伸）、大小（120）、修改时间（160）；文件夹在前、同名按名称升序；双击文件夹进入并更新面包屑 |
| 面包屑 | 位于列表上方，形如"我的网盘 / 文档"，可点击上级节点返回 |
| 底部状态栏 | 左显示"已用 X / 5.0 GB"，右侧 120×12 的进度条按用量填充 |
| 错误提示 | 操作失败在状态栏左侧显示红色文字 3 秒后恢复；401 时提示"登录已过期"并自动返回登录窗 |

---

## Task 1：客户端工程骨架与 ApiClient

**目标：** 建立 qmake 工程，封装与阶段 1 接口一致的 HTTP 客户端，并用 QtTest 覆盖 URL 拼接、错误映射、token 头。

**文件：**
- Create：`client/wangpan.pro`
- Create：`client/src/main.cpp`
- Create：`client/src/apiclient.h`、`client/src/apiclient.cpp`
- Create：`client/tests/test_apiclient.pro`
- Create：`client/tests/test_apiclient.cpp`

**步骤：**

- [x] 先写 `client/tests/test_apiclient.cpp`，覆盖三件事（此时应因 `ApiClient` 未实现而编译失败）：

```cpp
#include <QtTest>
#include "apiclient.h"

class TestApiClient : public QObject {
    Q_OBJECT
private slots:
    void buildsFilesUrl() {
        ApiClient c;
        c.setBaseUrl("http://127.0.0.1:18080");
        QCOMPARE(c.filesUrl(0), QString("http://127.0.0.1:18080/api/files?parent_id=0"));
        QCOMPARE(c.filesUrl(7), QString("http://127.0.0.1:18080/api/files?parent_id=7"));
    }
    void stripsTrailingSlash() {
        ApiClient c;
        c.setBaseUrl("http://127.0.0.1:18080/");
        QCOMPARE(c.filesUrl(0), QString("http://127.0.0.1:18080/api/files?parent_id=0"));
    }
    void mapsErrorBody() {
        ApiClient c;
        QCOMPARE(c.errorTextFor(409, "{\"error\":\"username_taken\",\"message\":\"该用户名已被使用\"}", QNetworkReply::NoError),
                 QString("该用户名已被使用"));
        QCOMPARE(c.errorTextFor(0, "", QNetworkReply::ConnectionRefusedError),
                 QString("无法连接服务器，请检查地址"));
        QCOMPARE(c.errorTextFor(401, "{\"error\":\"unauthorized\"}", QNetworkReply::NoError),
                 QString("登录已过期，请重新登录"));
    }
};
QTEST_MAIN(TestApiClient)
#include "test_apiclient.moc"
```

- [x] 写 `client/tests/test_apiclient.pro`：

```qmake
QT += testlib network
CONFIG += testcase console
TARGET = test_apiclient
TEMPLATE = app
INCLUDEPATH += ../src
SOURCES += test_apiclient.cpp ../src/apiclient.cpp
HEADERS += ../src/apiclient.h
```

- [x] 写 `client/wangpan.pro`（**Task 1 先只列 `main.cpp` 与 `apiclient.cpp`**，Task 2/3 新增的 `.cpp` 逐个加进来）：

```qmake
QT += core gui widgets network
CONFIG += c++11
TARGET = wangpan
TEMPLATE = app
INCLUDEPATH += src
SOURCES += src/main.cpp src/apiclient.cpp
HEADERS += src/apiclient.h
```

- [x] 写 `apiclient.h`：`explicit ApiClient(QObject* parent = nullptr)`、`setBaseUrl`、`setToken`、`filesUrl(qlonglong)`、`errorTextFor(int status, const QString& body, QNetworkReply::NetworkError err)`，以及异步方法 `login(user, pass)`、`registerUser(user, pass)`、`logout()`、`listFiles(qlonglong parentId)`、`createFolder(qlonglong parentId, const QString& name)`；对应信号 `loginSucceeded(qint64 userId, qint64 expiresAt)`、`registered(...)`、`filesLoaded(QJsonArray items, qint64 usedBytes, qint64 quotaBytes)`、`folderCreated(qint64 id, QString finalName)`、`failed(QString message)`、`unauthorized()`。
- [x] 写 `main.cpp`（Task 1 版本，只验证工程能跑）：`QApplication` + 一个 400×300 的 `QWidget`，标题"网盘 · 客户端骨架"，内含文字"等待登录界面"。Task 2 时整体替换为登录窗启动逻辑。
- [x] 编译并跑测试：

```powershell
$env:PATH = "D:\20250519\QT\Qt5.12.11\5.12.11\mingw73_64\bin;D:\20250519\QT\Qt5.12.11\Tools\mingw730_64\bin;" + $env:PATH
cd c:\Users\bzp20\Desktop\wangpan\client\tests
qmake test_apiclient.pro CONFIG+=release
mingw32-make -j4
.\release\test_apiclient.exe
```
  - 预期：`Totals: 3 passed, 0 failed`，退出码 0。

**安全说明：** token 只存内存，不写磁盘；密码不落盘、不进日志；HTTP 为明文，按 M3 已确认，客户端不自行拼接任何文件路径。

---

## Task 2：登录 / 注册窗口（百度网盘式交互）

**目标：** 按上面的设计契约实现登录与注册，两态在同一窗口内切换，错误与加载态可见。

**文件：**
- Create：`client/src/ui/theme.h`、`client/src/ui/theme.cpp`
- Create：`client/src/ui/loginwindow.h`、`client/src/ui/loginwindow.cpp`
- Modify：`client/src/main.cpp`（登录成功后打开主窗口）

**步骤：**

- [x] 写 `theme.h/.cpp`：集中定义颜色常量（`kPrimary #2D8CF0`、`kPrimaryHover #1E6FD9`、`kPrimaryDisabled #A0CFFF`、`kText #1F2329`、`kTextMuted #86909C`、`kBorder #D9D9D9`、`kErrorText #E64545`、`kErrorBg #FFF1F0`、`kBrandFrom #F5F8FF`、`kBrandTo #EAF2FF`）与尺寸常量（`kWindowW 760`、`kWindowH 440`、`kBrandW 320`、`kInputH 40`、`kInputW 360`、`kMargin 40`），供登录窗与主窗口复用。
- [x] 左侧品牌区用 `QWidget` 重写 `paintEvent` 画垂直渐变（`QLinearGradient`），文字用两个 `QLabel`；不加载任何外部图片。
- [x] 右侧按契约搭建：页签（两个 `QPushButton` 做扁平样式，"扫码登录"禁用并设 `setToolTip("暂未开放")`）、`QLineEdit` 用户名与密码（密码框 `setEchoMode(QLineEdit::Password)`）、"显示/隐藏"按钮（点击在 `Password` 与 `Normal` 之间切换并同步按钮文字）、错误条 `QLabel`（默认 `hide()`）、主按钮、底部链接行。
- [x] 实现状态机：`submit()` 时禁用输入与按钮、按钮文字改"登录中…"；`failed` 时恢复并显示错误条；`loginSucceeded` 时保存用户名与地址到 `QSettings`，发射 `loggedIn(token, userId)` 供 `main.cpp` 打开主窗口。
- [x] 注册态切换：隐藏页签、显示确认密码、按钮文字"注 册"、底部"返回登录"；两次密码不一致时本地提示"两次输入的密码不一致"，不发请求。
- [x] "设置服务器"链接：`QInputDialog::getText` 输入基地址，校验以 `http://` 或 `https://` 开头，非法时错误条提示"地址必须以 http:// 或 https:// 开头"。
- [x] 编译运行，手工验证：
  - 启动后窗口居中、760×440；
  - 输入错误密码 → 错误条显示"账号或密码错误"；
  - 服务器地址改为 `http://127.0.0.1:9999` → 显示"无法连接服务器，请检查地址"；
  - 点"注册账号" → 变为注册态；注册 `alice` 成功后自动登录。

**视觉验证：** 记录三张截图到 `docs/ui-shots/`：`phase2-login-idle.png`、`phase2-login-error.png`、`phase2-register.png`；检查页签下划线对齐、输入框宽 360 且左右留白 40、错误条出现时按钮位置不下移（错误条占位固定）。

---

## Task 3：主窗口（左目录树 + 右文件列表）

**目标：** 登录后展示根目录内容，可进入子目录、回到上级、新建文件夹，底部显示配额。

**文件：**
- Create：`client/src/ui/mainwindow.h`、`client/src/ui/mainwindow.cpp`
- Modify：`client/src/main.cpp`

**步骤：**

- [x] 搭建 `QSplitter`：左侧 `QTreeView`（固定初宽 240）承载文件夹树，右侧为面包屑 `QHBoxLayout` + `QTableView` + 底部状态栏。
- [x] 文件列表用 `QStandardItemModel` 三列；`filesLoaded` 时清空重填：`kind=="folder"` 的行名称前加"📁"前缀（纯文本图标，避免引入图片资源），大小列对文件夹显示"—"；大小用 `B/KB/MB/GB` 保留一位小数。
- [x] 双击文件夹行：从该行 `Qt::UserRole` 取 `id`，调用 `listFiles(id)`，并更新面包屑为"我的网盘 / 文档 / …"；面包屑每一级按钮同样携带 `parent_id`，点击返回。
- [x] "新建文件夹"：`QInputDialog::getText`，非空才发请求；成功后用服务端返回的 `finalName` 刷列表并选中该行（服务端按 R2 可能已自动改名，客户端不要用自己提交的名称）。
- [x] "刷新"重新请求当前目录；"退出登录"调用 `logout()` 后关闭主窗口并重新显示登录窗。
- [x] 状态栏：`已用 {used} / 5.0 GB` + `QProgressBar`（范围 0–100，按 `used/quota` 计算，>100% 时钳到 100 并变红）。
- [x] 401 处理：`unauthorized()` 信号 → 弹提示"登录已过期，请重新登录" → 返回登录窗。
- [x] 编译运行，与服务端联调：
  - 登录 alice → 列表空 → 新建"文档" → 列表出现"文档" → 双击进入 → 面包屑变"我的网盘 / 文档" → 刷新仍为空。

**视觉验证：** 截图 `phase2-main-empty.png`、`phase2-main-folder.png`；在 1024×640 与 1366×768 两种尺寸下检查工具栏按钮不换行、面包屑不截断、状态栏不与进度条重叠。

---

## Task 4：错误态与边界联调

**目标：** 客户端在服务端异常、越权、重名、token 失效时给出正确提示，不崩溃、不静默失败。

**文件：**
- Modify：`client/src/apiclient.cpp`（超时与错误映射）
- Modify：`client/src/ui/mainwindow.cpp`（状态栏提示）
- Modify：`client/tests/test_apiclient.cpp`（新增映射用例）

**步骤：**

- [x] 为所有请求设置 10 秒超时（`QNetworkRequest` + `QTimer` 或 `reply->setProperty` 计时），超时提示"请求超时，请检查网络"。
- [x] 在 `test_apiclient.cpp` 追加用例：`errorTextFor(413, "{\"error\":\"payload_too_large\"}", QNetworkReply::NoError)` 期望"内容过大，请检查输入"；`errorTextFor(500, "", QNetworkReply::NoError)` 期望"服务器内部错误，请稍后重试"。先运行，预期失败，再实现映射。
- [x] 手工验证清单（逐项打勾）：
  - 停掉服务端后点"刷新"→ 状态栏"无法连接服务器，请检查地址"，界面不卡死；
  - 用 alice 登录后，停止服务端并删除 `/tmp/wp-test/wangpan.db`（会话与账户一并清空，等价于 token 失效），重启服务端 → 点刷新 → 服务端返回 401 → 客户端提示"登录已过期"并回登录窗；
  - 新建同名文件夹两次 → 第二次列表出现"文档 (1)"（服务端按 R2 自动改名，不报错）；
  - 用户名输入 2 个字符点注册 → 提示"用户名需 3–20 个字符"（客户端与服务端提示一致）。
- [x] 运行测试，确认 5 条用例全过。

**安全说明：** 错误提示只展示服务端 `message` 或预设文案，不展示原始 JSON、URL 带 token、堆栈信息。

---

## Task 5：配置持久化、文档与整体验收

**目标：** 重启客户端后保留服务器地址与用户名；补文档；完成本阶段验收。

**文件：**
- Create：`client/README.md`
- Modify：`client/src/main.cpp`（启动时恢复设置）

**步骤：**

- [x] `QSettings` 键固定为 `server/baseUrl`、`login/lastUsername`；启动时读回并填入登录窗；确认关闭再打开后仍生效。
- [x] 写 `client/README.md`：环境变量 PATH 设置、qmake 与 mingw32-make 命令、Qt Creator 打开方式（打开 `client/wangpan.pro`，选 Desktop Qt 5.12.11 MinGW 64-bit 套件，直接运行）、服务器地址修改方式、已知限制（HTTP 明文）。
- [x] 完整验收：服务端在 WSL 启动 → 客户端登录 → 新建文件夹 → 进入 → 退出登录 → 重新登录仍能看到该文件夹。
- [x] 截图汇总：确认 `docs/ui-shots/` 下 5 张截图齐全，且与设计契约一致（颜色、尺寸、状态）。

---

## 阶段完成清单

- [x] 5 个任务全部完成
- [x] `test_apiclient.exe` 5 条用例全过
- [x] 登录、注册、错误提示、加载态按契约工作
- [x] 目录列表、进入子目录、新建文件夹、退出登录全部联调通过
- [x] 1024×640 与 1366×768 下无换行、遮挡、截断
- [x] 5 张截图归档在 `docs/ui-shots/`
- [x] 客户端未保存密码，未引入任何第三方品牌素材

## 回滚

客户端为独立工程，回滚只需删除 `client/`（或 `git checkout -- client/`）；不影响服务端与数据库。

## 验证命令汇总

```powershell
$env:PATH = "D:\20250519\QT\Qt5.12.11\5.12.11\mingw73_64\bin;D:\20250519\QT\Qt5.12.11\Tools\mingw730_64\bin;" + $env:PATH

# 单元测试
cd c:\Users\bzp20\Desktop\wangpan\client\tests
qmake test_apiclient.pro CONFIG+=release
mingw32-make -j4
.\release\test_apiclient.exe

# 客户端
cd c:\Users\bzp20\Desktop\wangpan\client
qmake wangpan.pro CONFIG+=release
mingw32-make -j4
.\release\wangpan.exe

# 服务端（另一个窗口）
wsl -d Ubuntu -- bash -lc "cd /mnt/c/Users/bzp20/Desktop/wangpan && ./build-wsl/wangpan_server --port 18080 --data-dir /tmp/wp-test"
```
- 若 Windows 无法访问 `127.0.0.1:18080`，把客户端"设置服务器"改为 `wsl hostname -I` 输出的地址加 `:18080`。

## 阻塞问题

- 登录窗的左侧品牌区只放"网盘"文字与副标题，不放任何第三方 Logo；如需换成你自己的名字/图标，实现前告知。
- 若你希望登录窗与百度网盘视觉更接近（例如圆角输入框、更接近的按钮色值），请在 Task 2 开始前给出具体修改点；默认按本契约实现。
