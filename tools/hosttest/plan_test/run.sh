#!/usr/bin/env bash
# Plan 的 host 測試（不需要板子）
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"   # tools/hosttest/<name>/ → repo root
OUT="${TMPDIR:-/tmp}/plan_test"
cc -std=c11 -Wall -Wextra -Werror \
   -DNC_PLAN_HOST_TEST \
   -I "$ROOT/tools/hosttest/plan_test" \
   -I "$ROOT/firmware/components/sys/mem/include" \
   "$ROOT/firmware/components/sys/mem/nc_plan.c" \
   "$ROOT/tools/hosttest/plan_test/plan_host.c" \
   "$ROOT/tools/hosttest/plan_test/main.c" \
   -o "$OUT"
"$OUT"
