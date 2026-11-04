# 阶段 6：部署到阿里云与客户端打包

**目标：** 服务端在阿里云 Ubuntu 22.04 上由 systemd 常驻、Nginx 转发、重启后自动恢复；Windows 客户端打包成免安装文件夹，用公网地址完成一次真实上传与下载。
**前置条件：** 阶段 1–5 完成，本机 `scripts/smoke.sh` 全绿；已有一台阿里云 ECS（Ubuntu 22.04，安全组可改）。
**产出：** `deploy/wangpan.service`、`deploy/nginx.conf`、`deploy/install.sh`、`deploy/README.md`、`scripts/package-client.ps1`、`dist/Wangpan-1.0.0-win64.zip`。

**本阶段界面决策：** 无界面改动。客户端只做打包，不改交互。

**范围边界：** 按 M3 不上 HTTPS；不配置域名与证书；不做备份任务（PRD 已将备份列为上线后第一件事）；不引入 Docker。

---

## Task 1：服务器初始化（用户、目录、依赖、安全组）

**目标：** 服务器具备编译与运行条件，且只有 80 端口对外。

**文件：**
- Create：`deploy/install.sh`（前半段：环境）
- Create：`deploy/README.md`（操作手册，含每一步命令与预期输出）

**步骤：**

- [ ] 阿里云控制台安全组入方向：`22` 仅放行自己当前公网 IP；`80` 放行 `0.0.0.0/0`；**不要放行 8080**（服务端只监听 127.0.0.1）。
- [ ] SSH 登录后安装依赖：

```bash
sudo apt update
sudo apt install -y build-essential cmake libsqlite3-dev libssl-dev nginx curl
```

- [ ] 创建运行账户与目录（`deploy/install.sh` 幂等执行）：

```bash
id -u wangpan >/dev/null 2>&1 || sudo useradd --system --home /var/lib/wangpan --shell /usr/sbin/nologin wangpan
sudo mkdir -p /opt/wangpan/bin /var/lib/wangpan/data
sudo chown -R wangpan:wangpan /var/lib/wangpan
sudo chmod 750 /var/lib/wangpan /var/lib/wangpan/data
```

- [ ] 从 Windows 上传源码（在项目根目录执行，`<IP>` 换成公网地址）：

```powershell
scp -r server deploy scripts ubuntu@<IP>:~/wangpan-src/
```

- [ ] 在服务器上构建并把二进制放进 `/opt/wangpan/bin`：

```bash
cd ~/wangpan-src
cmake -S server -B /tmp/wangpan-build -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/wangpan-build -j"$(nproc)"
sudo install -m 755 /tmp/wangpan-build/wangpan_server /opt/wangpan/bin/wangpan_server
```

- [ ] 手工前台启动验证（确认 Release 版可用）：

```bash
sudo -u wangpan /opt/wangpan/bin/wangpan_server --port 18080 --bind 127.0.0.1 --data-dir /var/lib/wangpan/data
# 另一个终端：curl -s http://127.0.0.1:18080/api/health
```

  - 预期：`{"ok":true,"version":"1.0"}`；数据目录下出现 `wangpan.db`。

**安全说明：** 服务端以 `wangpan` 系统用户运行，不监听公网；数据目录 750，只有该用户可读写。

---

## Task 2：systemd 常驻与日志

**目标：** 开机自启、崩溃自动重启、日志可查、重启后数据与登录态仍在。

**文件：**
- Create：`deploy/wangpan.service`
- Modify：`deploy/install.sh`（安装 unit、`daemon-reload`、`enable --now`）

**步骤：**

- [ ] 写 `deploy/wangpan.service`：

```ini
[Unit]
Description=Wangpan Server
After=network.target

[Service]
Type=simple
User=wangpan
Group=wangpan
ExecStart=/opt/wangpan/bin/wangpan_server --port 18080 --bind 127.0.0.1 --data-dir /var/lib/wangpan/data
Restart=on-failure
RestartSec=3
LimitNOFILE=65536
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=full
ReadWritePaths=/var/lib/wangpan

[Install]
WantedBy=multi-user.target
```

- [ ] 安装并启动：

```bash
sudo cp /root/wangpan-src/deploy/wangpan.service /etc/systemd/system/wangpan.service 2>/dev/null || \
  sudo cp ~/wangpan-src/deploy/wangpan.service /etc/systemd/system/wangpan.service
sudo systemctl daemon-reload
sudo systemctl enable --now wangpan
sudo systemctl status wangpan --no-pager
```

- [ ] 验证崩溃自愈：`sudo pkill -9 wangpan_server` → 3 秒内 `systemctl is-active wangpan` 回到 `active`。
- [ ] 验证重启保留：重启服务前用 curl 注册 `persist_<时间戳>` 拿 token；`sudo reboot` 后重新登录同一账户能拿到新 token，旧 token 仍可用（会话 7 天）。
- [ ] 日志：`journalctl -u wangpan -n 50 --no-pager`，确认日志里没有密码、token、请求体。

**安全 / 边界说明：** `ProtectSystem=full` + `ReadWritePaths` 让程序只能写数据目录；`PrivateTmp=true` 让上传临时分片不出现在系统 `/tmp` 之外（`/tmp/wp-test` 这种测试路径仅用于本地调试，生产使用 `/var/lib/wangpan/data`）。

---

## Task 3：Nginx 反向代理、限流与大文件参数

**目标：** 客户端只访问 `http://<公网IP>/api/...`；4 MiB 分片可正常通过；登录/注册有基础限流；下载不被缓冲拖慢。

**文件：**
- Create：`deploy/nginx.conf`
- Modify：`deploy/install.sh`（安装配置、`nginx -t`、`reload`）

**步骤：**

- [ ] 写 `deploy/nginx.conf`（放到 `/etc/nginx/conf.d/wangpan.conf`，该目录本身就在 `http {}` 内）：

```nginx
limit_req_zone $binary_remote_addr zone=wp_auth:10m rate=10r/s;

server {
    listen 80;
    server_name _;

    client_max_body_size 6m;          # 4 MiB 分片 + 头开销；超过返回 413
    proxy_http_version 1.1;
    proxy_set_header Host $host;
    proxy_set_header X-Real-IP $remote_addr;
    proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
    proxy_set_header Connection "";

    # 上传分片/下载不要先落到 Nginx 临时文件
    proxy_request_buffering off;
    proxy_buffering off;
    proxy_read_timeout 300s;
    proxy_send_timeout 300s;

    location /api/login    { limit_req zone=wp_auth burst=20 nodelay; proxy_pass http://127.0.0.1:18080; }
    location /api/register { limit_req zone=wp_auth burst=20 nodelay; proxy_pass http://127.0.0.1:18080; }
    location /             { proxy_pass http://127.0.0.1:18080; }
}
```

- [ ] 安装并校验：

```bash
sudo cp ~/wangpan-src/deploy/nginx.conf /etc/nginx/conf.d/wangpan.conf
sudo nginx -t
sudo systemctl reload nginx
```

- [ ] 验证：
  - `curl -i http://<公网IP>/api/health` → 200 且 body 为 `{"ok":true,"version":"1.0"}`；
  - `curl -i -X POST http://<公网IP>/api/login -d '{}'` 连续 30 次，超过突发额度出现 `503`（限流生效，属预期）；
  - 用 7 MiB 单次 body 调 `PUT /api/upload/<sid>/chunks/0` → Nginx 返回 `413`（客户端实际只发 4 MiB，不受影响）；
  - `sudo ss -lntp | grep -E ':80|:18080'`：80 对外、18080 只绑 `127.0.0.1`。

**安全说明：** 只开 80（M3 已接受明文风险）；限流只挡暴力注册/登录，不替代 HTTPS。

---

## Task 4：客户端免安装打包

**目标：** 把 `wangpan.exe` 与 Qt 运行库打成 zip，解压即用，不依赖目标机的 Qt。

**文件：**
- Create：`scripts/package-client.ps1`
- Create：`deploy/README.md`（客户端分发说明）

**步骤：**

- [ ] 写脚本（在项目根目录执行 `powershell -ExecutionPolicy Bypass -File scripts/package-client.ps1 -Version 1.0.0`）：

```powershell
param([string]$Version = "1.0.0")
$ErrorActionPreference = "Stop"
$qtBin = "D:\20250519\QT\Qt5.12.11\5.12.11\mingw73_64\bin"
$env:PATH = "$qtBin;D:\20250519\QT\Qt5.12.11\Tools\mingw730_64\bin;" + $env:PATH

$root = "c:\Users\bzp20\Desktop\wangpan"
$dist = Join-Path $root "dist\Wangpan-$Version-win64"
$zip  = Join-Path $root "dist\Wangpan-$Version-win64.zip"
if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }
New-Item -ItemType Directory -Path $dist | Out-Null

Push-Location (Join-Path $root "client")
qmake wangpan.pro CONFIG+=release
mingw32-make -j4
Pop-Location

Copy-Item (Join-Path $root "client\release\wangpan.exe") $dist
& (Join-Path $qtBin "windeployqt.exe") --release --no-translations --compiler-runtime (Join-Path $dist "wangpan.exe")

Compress-Archive -Path $dist -DestinationPath $zip -Force
Write-Host "打包完成：$zip"
```

- [ ] 验证包内容包含：`wangpan.exe`、`Qt5Core/Gui/Widgets/Network.dll`、`platforms/qwindows.dll`、`styles/qwindowsvistastyle.dll`（或 `qwindowsstyle`）、`libgcc_s_seh-1.dll`、`libstdc++-6.dll`、`libwinpthread-1.dll`。
- [ ] 在**没有配置 Qt PATH** 的终端里从 `dist\Wangpan-1.0.0-win64\` 启动 `wangpan.exe`，确认能打开登录窗（不在 Qt Creator 里运行）。
- [ ] 把 zip 拷到另一台 Windows 机器（或另一个 Windows 用户）解压运行，确认同样能启动。
- [ ] 客户端"设置服务器"填入 `http://<公网IP>`，保存后重启客户端仍保留（QSettings 落在注册表 `HKCU\Software\Wangpan\Client`）。

**安全说明：** 包里不包含任何账号、token、服务器密码；`QSettings` 只存地址与用户名。

---

## Task 5：公网全链路验收与回滚演练

**目标：** 用真实公网走完注册 → 上传（暂停/继续）→ 预览 → 下载 → 回收站；验证重启与回滚。

**文件：**
- Create：`deploy/README.md`（补"验收清单"与"回滚步骤"）
- Modify：`server/src/api/health` 返回的 `version` 改为 `"1.0"`（若前面阶段仍是 `"0.1"`）
- Modify：`deploy/install.sh`（补备份旧二进制与数据）

**步骤：**

- [ ] 公网最小验证（用随机用户名，避免与已有账户冲突）：

```bash
SUF=$(date +%s)
curl -s -X POST http://<IP>/api/register -H 'Content-Type: application/json' \
  -d "{\"username\":\"deploy$SUF\",\"password\":\"pass12345\"}"
# 预期：201 + token
```

- [ ] 客户端（另一台机器）验收：
  - 注册新账号 → 自动登录 → 根目录为空；
  - 上传 100 MB 文件 → 中途暂停 → 继续 → 完成 → 列表大小正确；
  - 双击图片/文本 → 预览显示；PDF → "暂不支持预览"；
  - 下载 → 本地 sha256 与源文件一致；
  - 删除 → 回收站 → 恢复 → 彻底删除 → 配额回到删除前。
- [ ] 网络中断演练：上传到 50% 时禁用本机网卡 5 秒 → 恢复后任务显示"网络中断，已暂停"，点继续从断点续传（进度不归零）。
- [ ] 服务端重启：`sudo systemctl restart wangpan` → 客户端点刷新仍可用（token 7 天有效）；上传中任务按"暂停/继续"恢复。
- [ ] 服务端整机重启：`sudo reboot` → 等待 1 分钟 → `curl http://<IP>/api/health` 200 → 数据仍在。
- [ ] 回滚演练（先在测试实例上做一遍）：
  1. 备份 `sudo tar -czf /var/backups/wangpan-$(date +%F).tar.gz /var/lib/wangpan/data`；
  2. 保留旧二进制：`sudo cp /opt/wangpan/bin/wangpan_server /opt/wangpan/bin/wangpan_server.prev`；
  3. 回滚：`sudo systemctl stop wangpan` → `sudo cp /opt/wangpan/bin/wangpan_server.prev /opt/wangpan/bin/wangpan_server` → 恢复数据快照 → `sudo systemctl start wangpan`。
- [ ] 记录公网实测数据（上传速度、100 MB 用时）到 `deploy/README.md`，作为简历项目可量化的结果。

**安全 / 边界说明：** 生产库不要跑全量 `scripts/smoke.sh`（会创建固定账户并上传测试文件）；只跑最小验证段。验收产生的测试账户保留，不做删除（本期无账户注销接口）。

---

## 阶段完成清单

- [ ] 5 个任务全部完成
- [ ] `http://<公网IP>/api/health` 返回 200 且 `version` 为 `1.0`
- [ ] systemd `enabled` 且崩溃/重启能自愈
- [ ] Nginx 转发正常、4 MiB 分片可传、登录限流生效
- [ ] 8080 未对公网开放，18080 仅绑定 127.0.0.1
- [ ] 免安装 zip 在无 Qt 的机器上可启动并完成登录
- [ ] 公网完成一次上传（含断点续传）、预览、下载、回收站闭环
- [ ] 整机重启后数据与登录态保留
- [ ] 回滚步骤演练过一次并有备份文件
- [ ] `deploy/README.md` 含完整命令、验收清单、回滚步骤

## 回滚

- 代码回滚：恢复 `/opt/wangpan/bin/wangpan_server.prev` 并重启服务；
- 配置回滚：删除 `/etc/nginx/conf.d/wangpan.conf` 后 `nginx -t && systemctl reload nginx`，`systemctl disable --now wangpan`；
- 数据回滚：使用最近一次 `/var/backups/wangpan-*.tar.gz`，停服后解压覆盖 `/var/lib/wangpan/data`，再启服；
- 客户端回滚：删除 `dist/` 与 `scripts/package-client.ps1`，不影响已发布的旧 zip。

## 验证命令汇总

```powershell
# 客户端打包
powershell -ExecutionPolicy Bypass -File scripts/package-client.ps1 -Version 1.0.0

# 公网健康检查
curl.exe -s http://<IP>/api/health

# 观察服务状态（SSH 内）
sudo systemctl status wangpan --no-pager
sudo journalctl -u wangpan -n 50 --no-pager
sudo ss -lntp | Select-String -Pattern ':80|:18080' -SimpleMatch
sudo tail -n 30 /var/log/nginx/access.log
```
