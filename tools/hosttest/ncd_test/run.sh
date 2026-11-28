#!/usr/bin/env bash
# .nct / .nce manifest 解碼的 host 測試（不需要板子、不需要 IDF）
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
OUT="${TMPDIR:-/tmp}/ncd_test"
cc -std=c11 -Wall -Wextra -Werror \
   -I "$ROOT/firmware/components/sw/plugin/include" \
   "$ROOT/firmware/components/sw/plugin/nc_ncd.c" \
   "$ROOT/tools/hosttest/ncd_test/main.c" \
   -o "$OUT"
"$OUT"
