# 网盘 Windows 客户端（阶段 2）

Qt 5.12.11 Widgets + Qt Network 实现的桌面客户端，通过 HTTP + JSON 访问 `server/` 提供的服务端接口。本阶段包含登录 / 注册、目录浏览、新建文件夹与退出登录。

## 环境

- Qt 5.12.11（`mingw73_64`）与随附的 MinGW 7.3 工具链
- 本仓库默认路径：`D:\20250519\QT\Qt5.12.11\5.12.11\mingw73_64`、`D:\20250519\QT\Qt5.12.11\Tools\mingw730_64`

命令行构建前把两个 `bin` 目录加入 PATH（PowerShell）：

```powershell
$env:PATH = "D:\20250519\QT\Qt5.12.11\5.12.11\mingw73_64\bin;D:\20250519\QT\Qt5.12.11\Tools\mingw730_64\bin;" + $env:PATH
```

## 构建与运行

```powershell
cd client
qmake wangpan.pro CONFIG+=release
mingw32-make -j4
.\release\wangpan.exe
```

首次启动会使用默认服务器地址 `http://127.0.0.1:8080`；服务端默认监听 8080，若按 README 用 `--port 18080` 启动，请在登录窗点"设置服务器"改成对应地址。

## 单元测试

```powershell
cd client\tests
qmake test_apiclient.pro CONFIG+=release
mingw32-make -j4
.\release\test_apiclient.exe
```

覆盖：URL 拼接、基地址尾部斜杠、错误体映射、413 / 500 预设文案。

## Qt Creator

打开 `client/wangpan.pro`，选择 **Desktop Qt 5.12.11 MinGW 64-bit** 套件，直接运行即可（无需手工设置 PATH，Qt Creator 会自动带上工具链）。

## 修改服务器地址

两种方式等价：

1. 登录窗底部"设置服务器"，输入以 `http://` 或 `https://` 开头的地址；
2. 修改注册表 `HKEY_CURRENT_USER\Software\Wangpan\Client\server\baseUrl`。

地址只保存到 `QSettings("Wangpan", "Client")`，下次启动自动回填；**密码从不落盘**。

## 已知限制

- 本期通信为 HTTP 明文（已确认的取舍），公网部署前需 Nginx + TLS。
- token 只存在内存中，客户端退出即需要重新登录。
- 本阶段只支持目录浏览与新建文件夹；上传、下载、预览、回收站在后续阶段接入。
- 扫码登录按钮仅为占位，悬停提示"暂未开放"。
