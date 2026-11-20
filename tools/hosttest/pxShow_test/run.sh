#!/usr/bin/env bash
# pxShow_test — mode 委派 plan ＋ 逐幀合成（host）
set -euo pipefail
cd "$(dirname "$0")/../../.."

CC=${CC:-cc}
PXF=firmware/components/sw/pixel
T=tools/hosttest/pxShow_test
OUT="$T/build"
SHIM=tools/hosttest/pxLayer_test/include
mkdir -p "$OUT"

"$CC" -std=c17 -Wall -Wextra -Werror -O1 \
    -I"$PXF/include" -I"$SHIM" \
    "$PXF/asset/nc_px_json.c" \
    "$PXF/asset/nc_px_asset.c" \
    "$PXF/asset/nc_px_layer.c" \
    "$PXF/asset/nc_px_show.c" \
    "$PXF/engine/nc_px_engine_tables.c" \
    "$PXF/engine/nc_px_engine_program.c" \
    "$T/pxShow_test.c" -lm -o "$OUT/pxShow_test"
"$OUT/pxShow_test"
