#!/bin/sh
# nc_cfg_patch host test (POSIX). 需要 cc。
set -e
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
OUT="${TMPDIR:-/tmp}/cfg_test_build"
mkdir -p "$OUT"
cc -std=c11 -Wall -Wextra -Werror \
   -I"$ROOT/firmware/components/sys/config/include" \
   -I"$ROOT/firmware/components/sys/config" \
   "$ROOT/tools/hosttest/cfg_test/main.c" \
   "$ROOT/firmware/components/sys/config/nc_cfg_patch.c" \
   -o "$OUT/cfg_test"
"$OUT/cfg_test"
