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

# ===== 账户：注册 / 登录 / 登出 =====
Code=$(curl -s -o /tmp/wp_reg.json -w '%{http_code}' --max-time 5 \
  -H 'Content-Type: application/json' \
  -d '{"username":"alice","password":"alice12345"}' "$BASE/api/register")
check "register alice 201" 201 "$Code"
TOKEN=$(sed -n 's/.*"token":"\([0-9a-f]*\)".*/\1/p' /tmp/wp_reg.json)
check "register token 64 位" 64 "${#TOKEN}"

Code=$(curl -s -o /tmp/wp_dup.json -w '%{http_code}' --max-time 5 \
  -H 'Content-Type: application/json' \
  -d '{"username":"alice","password":"alice12345"}' "$BASE/api/register")
check "重名注册 409" 409 "$Code"
check_grep "重名提示" /tmp/wp_dup.json '该用户名已被使用'

Code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 \
  -H 'Content-Type: application/json' \
  -d '{"username":"alice","password":"wrongpass1"}' "$BASE/api/login")
check "密码错误 401" 401 "$Code"

Code=$(curl -s -o /tmp/wp_login.json -w '%{http_code}' --max-time 5 \
  -H 'Content-Type: application/json' \
  -d '{"username":"alice","password":"alice12345"}' "$BASE/api/login")
check "登录成功 200" 200 "$Code"
TOKEN2=$(sed -n 's/.*"token":"\([0-9a-f]*\)".*/\1/p' /tmp/wp_login.json)

# 注意：服务端使用 cpp-httplib，POST 必须带 Content-Length（这里用 --data '' 产生）。
# 不带请求体且不带 Content-Length 的 POST 会让连接停在读超时，客户端不应这样发。
Code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 -X POST --data '' \
  -H "Authorization: Bearer $TOKEN2" "$BASE/api/logout")
check "登出 204" 204 "$Code"
Code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 -X POST --data '' \
  -H "Authorization: Bearer $TOKEN2" "$BASE/api/logout")
check "登出后再用同一 token 401" 401 "$Code"

Code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 \
  -H 'Content-Type: application/json' \
  -d '{"username":"bob","password":"short7"}' "$BASE/api/register")
check "密码过短 400" 400 "$Code"

Code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 \
  -H 'Content-Type: application/json' \
  -d '{"username":"ab","password":"alice12345"}' "$BASE/api/register")
check "用户名过短 400" 400 "$Code"

BIG=$(printf 'a%.0s' $(seq 1 17000))
Code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 \
  -H 'Content-Type: application/json' \
  -d "{\"username\":\"carol\",\"password\":\"$BIG\"}" "$BASE/api/register")
check "请求体过大 413" 413 "$Code"

# ===== 目录：列表 / 新建文件夹 =====
Code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 "$BASE/api/files?parent_id=0")
check "无 token 列目录 401" 401 "$Code"

Code=$(curl -s -o /tmp/wp_mk.json -w '%{http_code}' --max-time 5 \
  -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  -d '{"parent_id":0,"name":"文档"}' "$BASE/api/folders")
check "新建文件夹 201" 201 "$Code"
check_grep "新建返回名称" /tmp/wp_mk.json '"name":"文档"'
FOLDER_ID=$(sed -n 's/.*"id":\([0-9]*\).*/\1/p' /tmp/wp_mk.json)

Code=$(curl -s -o /tmp/wp_mk2.json -w '%{http_code}' --max-time 5 \
  -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  -d '{"parent_id":0,"name":"文档"}' "$BASE/api/folders")
check "重名自动改名 201" 201 "$Code"
check_grep "重名追加 (1)" /tmp/wp_mk2.json '"name":"文档 (1)"'

Code=$(curl -s -o /tmp/wp_list.json -w '%{http_code}' --max-time 5 \
  -H "Authorization: Bearer $TOKEN" "$BASE/api/files?parent_id=0")
check "列根目录 200" 200 "$Code"
check_grep "列表含文档" /tmp/wp_list.json '"name":"文档"'
check_grep "列表含配额" /tmp/wp_list.json '"quota_bytes":5368709120'

Code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 \
  -H "Authorization: Bearer $TOKEN" "$BASE/api/files?parent_id=999999")
check "不存在的目录 404" 404 "$Code"

Code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 \
  -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  -d '{"parent_id":0,"name":"a/b"}' "$BASE/api/folders")
check "非法名称 400" 400 "$Code"

# ===== 越权与并发 =====
Code=$(curl -s -o /tmp/wp_bob.json -w '%{http_code}' --max-time 5 \
  -H 'Content-Type: application/json' \
  -d '{"username":"bob","password":"pass12345"}' "$BASE/api/register")
check "注册 bob 201" 201 "$Code"
BTOKEN=$(sed -n 's/.*"token":"\([0-9a-f]*\)".*/\1/p' /tmp/wp_bob.json)

Code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 \
  -H "Authorization: Bearer $BTOKEN" "$BASE/api/files?parent_id=$FOLDER_ID")
check "跨账户访问他人目录 404" 404 "$Code"

Code=$(curl -s -o /tmp/wp_bob_list.json -w '%{http_code}' --max-time 5 \
  -H "Authorization: Bearer $BTOKEN" "$BASE/api/files?parent_id=0")
check "bob 列根目录 200" 200 "$Code"
check_grep "bob 根目录为空" /tmp/wp_bob_list.json '"items":\[\]'

# 并发注册同一用户名：靠 users.username 唯一约束，只应有一个 201
rm -f /tmp/wp_conc_*.txt
for i in 1 2 3 4 5; do
  curl -s -o /dev/null -w '%{http_code}\n' --max-time 5 \
    -H 'Content-Type: application/json' \
    -d '{"username":"dave","password":"pass12345"}' "$BASE/api/register" \
    > "/tmp/wp_conc_$i.txt" &
done
wait
CREATED=$(cat /tmp/wp_conc_*.txt 2>/dev/null | grep -c '^201$')
check "并发注册仅成功 1 次" 1 "$CREATED"

echo "通过 $pass 失败 $fail"
[ "$fail" -eq 0 ]
