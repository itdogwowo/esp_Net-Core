#!/usr/bin/env bash
# nce_test — `.nce` 值流信箱（雙緩衝／序號／截短／0–255→12-bit）
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
OUT="${TMPDIR:-/tmp}/nce_test"
cc -std=c11 -Wall -Wextra -Werror \
   -I "$ROOT/tools/hosttest/nce_test/include" \
   -I "$ROOT/tools/hosttest/hostshim" \
   -I "$ROOT/firmware/components/sw/pixel/include" \
   "$ROOT/firmware/components/sw/pixel/nc_px_nce.c" \
   "$ROOT/tools/hosttest/hostshim/mod_host.c" \
   "$ROOT/tools/hosttest/nce_test/main.c" \
   -o "$OUT"
"$OUT"
