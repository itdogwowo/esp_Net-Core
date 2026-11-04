#!/usr/bin/env bash
# nc_epoch host 測試（不需要板子、不需要 IDF）
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
OUT="${TMPDIR:-/tmp}/epoch_test"
cc -std=c11 -Wall -Wextra -Werror \
   -I "$ROOT/firmware/components/sys/epoch/include" \
   "$ROOT/firmware/components/sys/epoch/nc_epoch.c" \
   "$ROOT/tools/hosttest/epoch_test/main.c" \
   -o "$OUT"
"$OUT"
