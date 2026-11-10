#!/usr/bin/env bash
# 任務管理器的 host 測試（不需要板子、不需要 IDF）
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
OUT="${TMPDIR:-/tmp}/task_test"
cc -std=c11 -Wall -Wextra -Werror \
   -D_POSIX_C_SOURCE=200809L -DNC_BUF_HOST_TEST \
   -I "$ROOT/tools/hosttest/task_test/include" \
   -I "$ROOT/firmware/components/sys/task/include" \
   -I "$ROOT/firmware/components/sys/task" \
   -I "$ROOT/firmware/components/sys/task_manager" \
   -I "$ROOT/firmware/components/sys/config/include" \
   -I "$ROOT/firmware/components/sys/nc4/include" \
   -I "$ROOT/firmware/components/sys/mem/include" \
   "$ROOT/firmware/components/sys/task_manager/nc_task.c" \
   "$ROOT/firmware/components/sys/mem/nc_buf.c" \
   "$ROOT/tools/hosttest/task_test/cfg_stub.c" \
   "$ROOT/tools/hosttest/task_test/stubs.c" \
   "$ROOT/tools/hosttest/task_test/main.c" \
   -o "$OUT"
"$OUT"
