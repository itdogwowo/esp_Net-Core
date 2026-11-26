#!/usr/bin/env bash
# stateSchema_test — schema registry（查表／型別／範圍／enum／讀寫 API）
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
   "$ROOT/firmware/components/sys/state/nc_state_api.c" \
   "$ROOT/firmware/components/sys/state/nc_state_schema.c" \
   "$ROOT/firmware/components/sys/state/nc_state_schema_data.c" \
   "$T/stateSchema_test.c" -o "$OUT/stateSchema_test"
"$OUT/stateSchema_test"
