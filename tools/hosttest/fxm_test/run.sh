#!/usr/bin/env bash
# .fxe manifest 解碼的 host 測試（不需要板子、不需要 IDF）
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
OUT="${TMPDIR:-/tmp}/fxm_test"
cc -std=c11 -Wall -Wextra -Werror \
   -I "$ROOT/firmware/components/app/fx/include" \
   "$ROOT/firmware/components/app/fx/nc_fx_manifest.c" \
   "$ROOT/tools/hosttest/fxm_test/main.c" \
   -o "$OUT"
"$OUT"
