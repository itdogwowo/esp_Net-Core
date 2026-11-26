#!/usr/bin/env bash
# stateFile_test — NCSTATE 檔案層（hash／存讀／.bak／壞檔／缺檔）
set -euo pipefail
T="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$T/../../.." && pwd)"
SHIM="$ROOT/tools/hosttest/pxLayer_test/include"
OUT="$T/build"
mkdir -p "$OUT"
# macOS：淨係 -D_POSIX_C_SOURCE 會令 <stdlib.h> 唔出 mkdtemp，要埋 _DARWIN_C_SOURCE。
cc -std=c17 -Wall -Wextra -Werror -O2 -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE \
   -I"$ROOT/firmware/components/sys/state/include" \
   -I"$SHIM" \
   "$ROOT/firmware/components/sys/state/nc_state_blob.c" \
   "$ROOT/firmware/components/sys/state/nc_state_file.c" \
   "$T/stateFile_test.c" -o "$OUT/stateFile_test"
"$OUT/stateFile_test"
