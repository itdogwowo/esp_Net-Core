#!/usr/bin/env bash
# stateStore_test — NCSTATE RAM store（set/get/對拍/容量/bytes）
set -euo pipefail
T="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$T/../../.." && pwd)"
SHIM="$ROOT/tools/hosttest/pxLayer_test/include"
OUT="$T/build"
mkdir -p "$OUT"
cc -std=c17 -Wall -Wextra -Werror -O2 \
   -I"$ROOT/firmware/components/sys/state/include" \
   -I"$SHIM" \
   "$ROOT/firmware/components/sys/state/nc_state_blob.c" \
   "$ROOT/firmware/components/sys/state/nc_state_store.c" \
   "$ROOT/firmware/components/sys/state/nc_state_read.c" \
   "$T/stateStore_test.c" -o "$OUT/stateStore_test"
"$OUT/stateStore_test"
