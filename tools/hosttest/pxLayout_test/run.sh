#!/usr/bin/env bash
# pxLayout_test — 排版編譯器（nc_px_layout）：sel→dst runs、反序／隔顆／重疊／500 組（host）
set -euo pipefail
cd "$(dirname "$0")/../../.."

CC=${CC:-cc}
PXF=firmware/components/sw/pixel
T=tools/hosttest/pxLayout_test
OUT="$T/build"
SHIM=tools/hosttest/pxLayer_test/include
mkdir -p "$OUT"

"$CC" -std=c17 -Wall -Wextra -Werror -O2 \
    -I"$PXF/include" -I"$SHIM" -I"tools/hosttest/pxEngine_test/include" \
    "$PXF/asset/nc_px_json.c" \
    "$PXF/asset/nc_px_asset.c" \
    "$PXF/asset/nc_px_layer.c" \
    "$PXF/asset/nc_px_show.c" \
    "$PXF/asset/nc_px_layout.c" \
    "$PXF/engine/nc_px_engine_tables.c" \
    "$PXF/engine/nc_px_engine_program.c" \
    "$T/pxLayout_test.c" -lm -o "$OUT/pxLayout_test"
"$OUT/pxLayout_test"
