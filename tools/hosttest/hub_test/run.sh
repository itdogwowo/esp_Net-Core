#!/usr/bin/env bash
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"   # tools/hosttest/<name>/ → repo root
OUT="${TMPDIR:-/tmp}/hub_test"
# -D_POSIX_C_SOURCE：nc_hub.c 的 host 分支用 posix_memalign()。glibc 在 -std=c11（嚴格模式）
# 之下會把這個宣告藏起來，於是變成 implicit declaration → -Werror 直接當錯。
# 目標（ESP-IDF）那邊有 IDF 自己的 stdlib，所以只有 host 測試要加這個 define。
cc -std=c11 -Wall -Wextra -Werror \
   -D_POSIX_C_SOURCE=200809L \
   -DNC_HUB_HOST_TEST \
   -I "$ROOT/tools/hosttest/hub_test" \
   -I "$ROOT/firmware/components/sys/mem/include" \
   "$ROOT/firmware/components/sys/mem/nc_hub.c" \
   "$ROOT/tools/hosttest/hub_test/main.c" \
   -o "$OUT"
"$OUT"
