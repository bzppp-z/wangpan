# 网盘服务端（阶段 1）

C++14 + cpp-httplib + SQLite3 + OpenSSL 的单机多用户网盘服务端。本阶段只包含账户与目录能力，文件上传在阶段 3 接入。

## 依赖

Ubuntu 22.04 / WSL 中安装：

```bash
sudo apt update
sudo apt install -y build-essential cmake libsqlite3-dev libssl-dev curl
```

第三方头文件已随仓库提供，无需额外下载：

- `third_party/httplib.h`（cpp-httplib）
- `third_party/nlohmann/json.hpp`

## 构建

```bash
cd /mnt/c/Users/bzp20/Desktop/wangpan
cmake -S server -B build-wsl -DCMAKE_BUILD_TYPE=Debug
cmake --build build-wsl -j
```

产物：`build-wsl/wangpan_server`。

## 启动

```bash
./build-wsl/wangpan_server --port 18080 --data-dir /tmp/wp-test
```

| 参数 | 默认值 | 说明 |
|---|---|---|
| `--port` | 8080 | 监听端口，取值 1–65535 |
| `--bind` | 127.0.0.1 | 监听地址，默认只允许本机访问 |
| `--data-dir` | ./data | 数据目录，自动创建 |

启动时会自动建库并幂等建表，数据文件为 `<data-dir>/wangpan.db`（WAL 模式下还会有 `-wal`、`-shm`）。

## 测试

```bash
# 单元测试：建表、用户、鉴权
ctest --test-dir build-wsl --output-on-failure

# 端到端冒烟：需要先启动服务端，且使用全新 data-dir
bash scripts/smoke.sh http://127.0.0.1:18080
```

冒烟脚本要求干净的数据库（会注册固定用户名），每次运行前请 `rm -rf` 数据目录后重启服务端。

## 接口（阶段 1）

| 方法与路径 | 鉴权 | 说明 |
|---|---|---|
| `GET /api/health` | 否 | 健康检查，返回 `{"ok":true,"version":"0.1"}` |
| `POST /api/register` | 否 | 注册并直接返回会话，201 |
| `POST /api/login` | 否 | 登录，200 |
| `POST /api/logout` | Bearer | 删除当前会话，204 |
| `GET /api/files?parent_id=N` | Bearer | 子项列表 + `used_bytes` + `quota_bytes` |
| `POST /api/folders` | Bearer | 新建文件夹，重名自动改名，201 |

错误响应统一为 `{"error":"<code>","message":"<中文提示>"}`。

## 约定与限制

- 所有 POST 必须带 `Content-Length`（无内容时为 0）。cpp-httplib 对"无请求体且无 Content-Length"的 POST 会让连接停在读超时。
- 密码用 PBKDF2-HMAC-SHA256（10 万次迭代）+ 32 字节随机 salt 存储；数据库中只有 salt 与散列结果。
- token 为 64 位十六进制随机串，库里只存其 SHA-256，有效期 7 天；登出即删除。
- 所有文件 / 文件夹查询都带 `owner_id`，跨账户访问一律 404。
- 本期使用 HTTP 明文（已确认的取舍），部署到公网前需加 Nginx + TLS。
- 单机 SQLite，无备份；多机扩展需换元数据库与对象存储。
