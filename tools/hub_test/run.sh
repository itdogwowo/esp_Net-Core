#!/usr/bin/env bash
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${TMPDIR:-/tmp}/hub_test"
cc -std=c11 -Wall -Wextra -Werror \
   -DNC_HUB_HOST_TEST \
   -I "$ROOT/tools/hub_test" \
   -I "$ROOT/components/sys/mem/include" \
   "$ROOT/components/sys/mem/nc_hub.c" \
   "$ROOT/tools/hub_test/main.c" \
   -o "$OUT"
"$OUT"
