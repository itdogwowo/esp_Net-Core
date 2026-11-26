#!/usr/bin/env bash
# stateFile_test — NCSTATE 檔案層（hash／存讀／.bak／壞檔／缺檔）
set -euo pipefail
T="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$T/../../.." && pwd)"
SHIM="$ROOT/tools/hosttest/pxLayer_test/include"
OUT="$T/build"
mkdir -p "$OUT"
cc -std=c17 -Wall -Wextra -Werror -O2 -D_POSIX_C_SOURCE=200809L \
   -I"$ROOT/firmware/components/sys/state/include" \
   -I"$SHIM" \
   "$ROOT/firmware/components/sys/state/nc_state_blob.c" \
   "$ROOT/firmware/components/sys/state/nc_state_file.c" \
   "$T/stateFile_test.c" -o "$OUT/stateFile_test"
"$OUT/stateFile_test"
