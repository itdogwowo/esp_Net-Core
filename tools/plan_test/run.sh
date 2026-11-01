#!/usr/bin/env bash
# Plan 的 host 測試（不需要板子）
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${TMPDIR:-/tmp}/plan_test"
cc -std=c11 -Wall -Wextra -Werror \
   -DNC_PLAN_HOST_TEST \
   -I "$ROOT/tools/plan_test" \
   -I "$ROOT/components/sys/mem/include" \
   "$ROOT/components/sys/mem/nc_plan.c" \
   "$ROOT/tools/plan_test/plan_host.c" \
   "$ROOT/tools/plan_test/main.c" \
   -o "$OUT"
"$OUT"
