#!/usr/bin/env bash
# stateBlob_test — NCSTATE codec host test（唯一狀態格式：自描述、CRC、向前兼容）
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
   "$T/stateBlob_test.c" -o "$OUT/stateBlob_test"
"$OUT/stateBlob_test"
