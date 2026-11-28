#!/usr/bin/env bash
# ncm 狀態機的 host 測試（不需要板子、不需要 IDF）
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
OUT="${TMPDIR:-/tmp}/mod_test"
cc -std=c11 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L \
   -DNC_MOD_HOST_TEST \
   -I "$ROOT/tools/hosttest/mod_test/include" \
   -I "$ROOT/firmware/components/host/ncm/include" \
   -I "$ROOT/firmware/components/sw/plugin/include" \
   "$ROOT/firmware/components/host/ncm/nc_mod.c" \
   "$ROOT/firmware/components/host/ncm/nc_res.c" \
   "$ROOT/firmware/components/sw/plugin/nc_plugin.c" \
   "$ROOT/firmware/components/sw/plugin/nc_ncd.c" \
   "$ROOT/firmware/components/sw/plugin/nc_meta.c" \
   "$ROOT/tools/hosttest/mod_test/mod_host.c" \
   "$ROOT/tools/hosttest/mod_test/main.c" \
   -o "$OUT"
"$OUT"
