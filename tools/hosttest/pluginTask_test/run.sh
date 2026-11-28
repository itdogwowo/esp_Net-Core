#!/usr/bin/env bash
# 插件工具 × Task_test 的 host 測試（唔需要板子／IDF／WAMR）
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
OUT="${TMPDIR:-/tmp}/pluginTask_test"
cc -std=c11 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L \
   -DNC_MOD_HOST_TEST \
   -I "$ROOT/tools/hosttest/hostshim" \
   -I "$ROOT/firmware/components/host/ncm/include" \
   -I "$ROOT/firmware/components/sw/plugin/include" \
   "$ROOT/firmware/components/host/ncm/nc_mod.c" \
   "$ROOT/firmware/components/host/ncm/nc_res.c" \
   "$ROOT/firmware/components/sw/plugin/nc_plugin.c" \
   "$ROOT/firmware/components/sw/plugin/nc_ncd.c" \
   "$ROOT/firmware/components/sw/plugin/nc_meta.c" \
   "$ROOT/tools/hosttest/hostshim/mod_host.c" \
   "$ROOT/tools/hosttest/pluginTask_test/main.c" \
   -o "$OUT"
"$OUT"
