#!/usr/bin/env bash
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"   # tools/hosttest/<name>/ → repo root
OUT="${TMPDIR:-/tmp}/hub_test"
cc -std=c11 -Wall -Wextra -Werror \
   -DNC_HUB_HOST_TEST \
   -I "$ROOT/tools/hosttest/hub_test" \
   -I "$ROOT/firmware/components/sys/mem/include" \
   "$ROOT/firmware/components/sys/mem/nc_hub.c" \
   "$ROOT/tools/hosttest/hub_test/main.c" \
   -o "$OUT"
"$OUT"
