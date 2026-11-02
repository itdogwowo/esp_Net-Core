#!/usr/bin/env bash
# 任務管理器的 host 測試（不需要板子、不需要 IDF）
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
OUT="${TMPDIR:-/tmp}/task_test"
cc -std=c11 -Wall -Wextra -Werror \
   -I "$ROOT/tools/hosttest/task_test/include" \
   -I "$ROOT/firmware/components/sys/tasks/include" \
   -I "$ROOT/firmware/components/sys/tasks" \
   -I "$ROOT/firmware/components/sys/nc4/include" \
   -I "$ROOT/firmware/components/sys/mem/include" \
   "$ROOT/firmware/components/sys/tasks/nc_task.c" \
   "$ROOT/tools/hosttest/task_test/stubs.c" \
   "$ROOT/tools/hosttest/task_test/main.c" \
   -o "$OUT"
"$OUT"
