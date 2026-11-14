#!/usr/bin/env bash
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"   # tools/hosttest/<name>/ → repo root
OUT="${TMPDIR:-/tmp}/buf_test"
# -D_POSIX_C_SOURCE：host 分支用 posix_memalign()（glibc 在 -std=c11 嚴格模式下會藏起宣告）
cc -std=c11 -Wall -Wextra -Werror \
   -D_POSIX_C_SOURCE=200809L \
   -DNC_BUF_HOST_TEST \
   -I "$ROOT/tools/hosttest/buf_test" \
   -I "$ROOT/firmware/components/sys/mem/include" \
   "$ROOT/firmware/components/sys/mem/nc_buf.c" \
   "$ROOT/tools/hosttest/buf_test/main.c" \
   -pthread \
   -o "$OUT"
"$OUT"
