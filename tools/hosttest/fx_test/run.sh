#!/usr/bin/env bash
# fx 狀態機的 host 測試（不需要板子、不需要 IDF）
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
OUT="${TMPDIR:-/tmp}/fx_test"
cc -std=c11 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L \
   -DNC_FX_HOST_TEST \
   -I "$ROOT/tools/hosttest/fx_test/include" \
   -I "$ROOT/firmware/components/app/fx/include" \
   "$ROOT/firmware/components/app/fx/nc_fx.c" \
   "$ROOT/firmware/components/app/fx/nc_fx_manifest.c" \
   "$ROOT/tools/hosttest/fx_test/fx_host.c" \
   "$ROOT/tools/hosttest/fx_test/main.c" \
   -o "$OUT"
"$OUT"
