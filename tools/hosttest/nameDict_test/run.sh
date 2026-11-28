#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
CC=${CC:-cc}
PXF=firmware/components/sw/pixel
T=tools/hosttest/nameDict_test
OUT="$T/build"
mkdir -p "$OUT"
"$CC" -std=c17 -Wall -Wextra -Werror -O1 \
    -I"$PXF/include" -I"tools/hosttest/pxAsset_test/include" \
    "$PXF/asset/nc_px_name_dict.c" \
    "$T/nameDict_test.c" \
    -o "$OUT/nameDict_test"
cd "$T"
./build/nameDict_test
