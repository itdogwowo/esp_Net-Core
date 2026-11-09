#!/usr/bin/env bash
# NC4 出口登記表 host 測試（不需要板子、不需要 IDF）
#
# 測的是 nc4_link.c 本身：登記/重名/滿了、整幀交出的三種結果
# （成功、短送、錯誤）、統計、JSON（含任意 cap 的截斷安全）。
# 用得到的地方：parser 轉寄（nc4.c）與 test_action 的 0xF013 FWD。
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
OUT="${TMPDIR:-/tmp}/link_test"
cc -std=c11 -Wall -Wextra -Werror \
   -I "$ROOT/firmware/components/sys/nc4/include" \
   -I "$ROOT/tools/hosttest/link_test/stub" \
   "$ROOT/firmware/components/sys/nc4/nc4_link.c" \
   "$ROOT/tools/hosttest/link_test/main.c" \
   -o "$OUT"
"$OUT"
