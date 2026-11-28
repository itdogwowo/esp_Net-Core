#!/usr/bin/env bash
# status_test — STATUS_GET 自註冊區塊（nc_status）
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
OUT="${TMPDIR:-/tmp}/status_test"
cc -std=c11 -Wall -Wextra -Werror \
   -I "$ROOT/tools/hosttest/hostshim" \
   -I "$ROOT/firmware/components/sys/nc4/include" \
   "$ROOT/firmware/components/sys/nc4/nc_status.c" \
   "$ROOT/tools/hosttest/hostshim/mod_host.c" \
   "$ROOT/tools/hosttest/status_test/main.c" \
   -o "$OUT"
"$OUT"
