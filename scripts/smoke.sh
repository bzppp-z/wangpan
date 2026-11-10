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

# ===== 上传初始化：新建会话 / 续传 / 参数校验 / 配额 =====
dd if=/dev/urandom of=/tmp/wp_big.bin bs=1M count=5 status=none
SHA=$(sha256sum /tmp/wp_big.bin | cut -d' ' -f1)
INIT=$(curl -s -o /tmp/wp_init.json -w '%{http_code}' --max-time 5 -X POST "$BASE/api/upload/init" \
  -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  -d "{\"parent_id\":0,\"name\":\"big.bin\",\"size\":5242880,\"sha256\":\"$SHA\"}")
check "upload init 200" 200 "$INIT"
SID=$(sed -n 's/.*"session_id":"\([0-9a-f]*\)".*/\1/p' /tmp/wp_init.json)
check "init session id 32 位" 32 "${#SID}"
check_grep "init chunk_count 2" /tmp/wp_init.json '"chunk_count":2'
check_grep "init instant false" /tmp/wp_init.json '"instant":false'
check_grep "init received 为空" /tmp/wp_init.json '"received":\[\]'

INIT2=$(curl -s -o /tmp/wp_init2.json -w '%{http_code}' --max-time 5 -X POST "$BASE/api/upload/init" \
  -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  -d "{\"parent_id\":0,\"name\":\"big.bin\",\"size\":5242880,\"sha256\":\"$SHA\"}")
check "重复 init 200" 200 "$INIT2"
SID2=$(sed -n 's/.*"session_id":"\([0-9a-f]*\)".*/\1/p' /tmp/wp_init2.json)
check "同名同 sha 返回同一会话" "$SID" "$SID2"

Code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 -X POST "$BASE/api/upload/init" \
  -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  -d "{\"parent_id\":0,\"name\":\"x.bin\",\"size\":1,\"sha256\":\"zzz\"}")
check "sha 非法 400" 400 "$Code"

Code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 -X POST "$BASE/api/upload/init" \
  -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  -d "{\"parent_id\":0,\"name\":\"x.bin\",\"size\":10737418241,\"sha256\":\"$SHA\"}")
check "超过单文件上限 400" 400 "$Code"

Code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 -X POST "$BASE/api/upload/init" \
  -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  -d "{\"parent_id\":0,\"name\":\"x.bin\",\"size\":6442450944,\"sha256\":\"$SHA\"}")
check "超配额 507" 507 "$Code"

# ===== 分片上传 / 状态 / 续传 / 完成 =====
split -b 4194304 -d -a 1 /tmp/wp_big.bin /tmp/wp_part_
P0=$(curl -s -o /dev/null -w '%{http_code}' --max-time 10 -X PUT "$BASE/api/upload/$SID/chunks/0" \
  -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/octet-stream' \
  --data-binary @/tmp/wp_part_0)
check "chunk 0 204" 204 "$P0"

curl -s -o /tmp/wp_status.json --max-time 5 "$BASE/api/upload/$SID" \
  -H "Authorization: Bearer $TOKEN"
check_grep "状态收到分片 0" /tmp/wp_status.json '"received":\[0\]'

REINIT=$(curl -s -o /tmp/wp_reinit.json -w '%{http_code}' --max-time 5 \
  -X POST "$BASE/api/upload/init" \
  -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  -d "{\"parent_id\":0,\"name\":\"big.bin\",\"size\":5242880,\"sha256\":\"$SHA\"}")
check "续传 re-init 200" 200 "$REINIT"
RSID=$(sed -n 's/.*"session_id":"\([0-9a-f]*\)".*/\1/p' /tmp/wp_reinit.json)
check "续传返回同一会话" "$SID" "$RSID"
check_grep "续传带 received" /tmp/wp_reinit.json '"received":\[0\]'

P1=$(curl -s -o /dev/null -w '%{http_code}' --max-time 10 -X PUT "$BASE/api/upload/$SID/chunks/1" \
  -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/octet-stream' \
  --data-binary @/tmp/wp_part_1)
check "chunk 1 204" 204 "$P1"

DONE=$(curl -s -o /tmp/wp_done.json -w '%{http_code}' --max-time 20 \
  -X POST --data '' "$BASE/api/upload/$SID/complete" -H "Authorization: Bearer $TOKEN")
check "complete 201" 201 "$DONE"
check_grep "complete size" /tmp/wp_done.json '"size":5242880'
check_grep "complete dedup false" /tmp/wp_done.json '"dedup":false'

Code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 "$BASE/api/upload/$SID" \
  -H "Authorization: Bearer $TOKEN")
check "完成后再查会话 404" 404 "$Code"

curl -s -o /tmp/wp_list2.json --max-time 5 "$BASE/api/files?parent_id=0" \
  -H "Authorization: Bearer $TOKEN"
check_grep "列表含 big.bin" /tmp/wp_list2.json '"name":"big.bin"'

# 秒传：本账户已拥有同一内容，改名再传应立即完成
INST=$(curl -s -o /tmp/wp_inst.json -w '%{http_code}' --max-time 5 -X POST "$BASE/api/upload/init" \
  -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  -d "{\"parent_id\":0,\"name\":\"big2.bin\",\"size\":5242880,\"sha256\":\"$SHA\"}")
check "秒传 201" 201 "$INST"
check_grep "秒传 instant true" /tmp/wp_inst.json '"instant":true'

# 同名再看一次：秒传仍然自动改名，不能出现两条同路径
INST2=$(curl -s -o /tmp/wp_inst2.json -w '%{http_code}' --max-time 5 -X POST "$BASE/api/upload/init" \
  -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  -d "{\"parent_id\":0,\"name\":\"big.bin\",\"size\":5242880,\"sha256\":\"$SHA\"}")
check "同名秒传 201" 201 "$INST2"
check_grep "同名秒传自动改名" /tmp/wp_inst2.json '"name":"big.bin (1)"'

# 跨账户：bob 没上传过该内容 → 不能秒传，必须走新会话
BOBINST=$(curl -s -o /tmp/wp_bobinst.json -w '%{http_code}' --max-time 5 \
  -X POST "$BASE/api/upload/init" \
  -H "Authorization: Bearer $BTOKEN" -H 'Content-Type: application/json' \
  -d "{\"parent_id\":0,\"name\":\"stolen.bin\",\"size\":5242880,\"sha256\":\"$SHA\"}")
check "跨账户不秒传 200" 200 "$BOBINST"
check_grep "跨账户 instant false" /tmp/wp_bobinst.json '"instant":false'
BSID=$(sed -n 's/.*"session_id":"\([0-9a-f]*\)".*/\1/p' /tmp/wp_bobinst.json)
curl -s -o /dev/null -w '%{http_code}' --max-time 5 -X DELETE "$BASE/api/upload/$BSID" \
  -H "Authorization: Bearer $BTOKEN" > /dev/null

# 取消后状态 404
head -c 1048576 /dev/urandom > /tmp/wp_small.bin
SHA2=$(sha256sum /tmp/wp_small.bin | cut -d' ' -f1)
CINIT=$(curl -s -o /tmp/wp_cinit.json -w '%{http_code}' --max-time 5 \
  -X POST "$BASE/api/upload/init" \
  -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  -d "{\"parent_id\":0,\"name\":\"cancel.bin\",\"size\":1048576,\"sha256\":\"$SHA2\"}")
check "待取消会话 init 200" 200 "$CINIT"
CSID=$(sed -n 's/.*"session_id":"\([0-9a-f]*\)".*/\1/p' /tmp/wp_cinit.json)

CANCEL=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 -X DELETE \
  "$BASE/api/upload/$CSID" -H "Authorization: Bearer $TOKEN")
check "取消会话 204" 204 "$CANCEL"
Code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 "$BASE/api/upload/$CSID" \
  -H "Authorization: Bearer $TOKEN")
check "取消后状态 404" 404 "$Code"

# 校验值不符：声明 sha 与实际分片内容不一致 → 400，且会话被丢弃
MINIT=$(curl -s -o /tmp/wp_minit.json -w '%{http_code}' --max-time 5 \
  -X POST "$BASE/api/upload/init" \
  -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  -d "{\"parent_id\":0,\"name\":\"bad.bin\",\"size\":1048576,\"sha256\":\"$SHA2\"}")
check "错误校验值会话 init 200" 200 "$MINIT"
MSID=$(sed -n 's/.*"session_id":"\([0-9a-f]*\)".*/\1/p' /tmp/wp_minit.json)
head -c 1048576 /dev/zero > /tmp/wp_zero.bin
curl -s -o /dev/null --max-time 10 -X PUT "$BASE/api/upload/$MSID/chunks/0" \
  -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/octet-stream' \
  --data-binary @/tmp/wp_zero.bin
MDONE=$(curl -s -o /tmp/wp_mdone.json -w '%{http_code}' --max-time 20 \
  -X POST --data '' "$BASE/api/upload/$MSID/complete" -H "Authorization: Bearer $TOKEN")
check "校验失败 400" 400 "$MDONE"
check_grep "校验失败提示" /tmp/wp_mdone.json 'checksum_mismatch'
Code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 "$BASE/api/upload/$MSID" \
  -H "Authorization: Bearer $TOKEN")
check "校验失败后会话已清理 404" 404 "$Code"

# ===== 下载：整文件 / Range 续传 / 越权 =====
fid=$(sed -n 's/.*"file_id":\([0-9]*\).*/\1/p' /tmp/wp_done.json)
FULL=$(curl -s -o /tmp/wp_dl_full.bin -D /tmp/wp_dl_full_hdr.txt -w '%{http_code}' --max-time 20 \
  "$BASE/api/files/$fid/content" -H "Authorization: Bearer $TOKEN")
check "download 200" 200 "$FULL"
check "download size" 5242880 "$(stat -c %s /tmp/wp_dl_full.bin)"
check "download sha 一致" "$SHA" "$(sha256sum /tmp/wp_dl_full.bin | cut -d' ' -f1)"
check_grep "download Accept-Ranges" /tmp/wp_dl_full_hdr.txt 'Accept-Ranges: bytes'
check_grep "download ETag" /tmp/wp_dl_full_hdr.txt "ETag: \"$SHA\""
check_grep "download Content-Disposition" /tmp/wp_dl_full_hdr.txt "filename\*=UTF-8''big.bin"

PART=$(curl -s -o /tmp/wp_dl_part.bin -D /tmp/wp_dl_part_hdr.txt -w '%{http_code}' --max-time 20 \
  -H "Authorization: Bearer $TOKEN" -H 'Range: bytes=1048576-' "$BASE/api/files/$fid/content")
check "range 206" 206 "$PART"
check_grep "content-range" /tmp/wp_dl_part_hdr.txt 'Content-Range: bytes 1048576-5242879/5242880'
check "range body size" 4194304 "$(stat -c %s /tmp/wp_dl_part.bin)"

# 后端返回 206 的片段必须与整文件同偏移一致
dd if=/tmp/wp_dl_full.bin of=/tmp/wp_dl_tail.bin bs=1M skip=1 status=none
check "range 内容一致" "$(sha256sum /tmp/wp_dl_tail.bin | cut -d' ' -f1)" \
  "$(sha256sum /tmp/wp_dl_part.bin | cut -d' ' -f1)"

# 终点越界按末尾截断：bytes=5000000-9000000 → 5000000-5242879
OOB=$(curl -s -o /tmp/wp_dl_oob.bin -D /tmp/wp_dl_oob_hdr.txt -w '%{http_code}' --max-time 20 \
  -H "Authorization: Bearer $TOKEN" -H 'Range: bytes=5000000-9000000' "$BASE/api/files/$fid/content")
check "越界终点 206" 206 "$OOB"
check "越界终点截断长度" 242880 "$(stat -c %s /tmp/wp_dl_oob.bin)"

check "bad range 416" 416 "$(curl -s -o /tmp/wp_dl_416.json -D /tmp/wp_dl_416_hdr.txt \
  -w '%{http_code}' --max-time 5 -H "Authorization: Bearer $TOKEN" \
  -H 'Range: bytes=5242880-' "$BASE/api/files/$fid/content")"
check_grep "416 带 Content-Range" /tmp/wp_dl_416_hdr.txt 'Content-Range: bytes \*/5242880'

check "cross account download 404" 404 "$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 \
  -H "Authorization: Bearer $BTOKEN" "$BASE/api/files/$fid/content")"
check "不存在文件 404" 404 "$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 \
  -H "Authorization: Bearer $TOKEN" "$BASE/api/files/999999/content")"
check "无 token 下载 401" 401 "$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 \
  "$BASE/api/files/$fid/content")"

echo "通过 $pass 失败 $fail"
[ "$fail" -eq 0 ]
