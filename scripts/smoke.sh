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
