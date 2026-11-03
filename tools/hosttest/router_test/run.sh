#!/usr/bin/env bash
# NC4 路由器 host 測試（不需要板子、不需要 IDF）
#
# 測的是「線 → 核」這張表本身：設定/驗證/查詢/保留核/JSON 輸出。
# 用得到的地方：main.c 開機時決定 decode0 / decode1 要唔要開。
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
OUT="${TMPDIR:-/tmp}/router_test"
cc -std=c11 -Wall -Wextra -Werror \
   -I "$ROOT/firmware/components/sys/nc4/include" \
   -I "$ROOT/tools/hosttest/router_test/stub" \
   "$ROOT/firmware/components/sys/nc4/nc4_router.c" \
   "$ROOT/tools/hosttest/router_test/main.c" \
   -o "$OUT"
"$OUT"
