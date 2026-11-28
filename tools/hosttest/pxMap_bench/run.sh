#!/usr/bin/env bash
# pxMap_bench — 配對／排版成本（host；唔係 pass/fail 測試）
set -euo pipefail
cd "$(dirname "$0")/../../.."

CC=${CC:-cc}
PXF=firmware/components/sw/pixel
T=tools/hosttest/pxMap_bench
OUT="$T/build"
SHIM=tools/hosttest/pxLayer_test/include
mkdir -p "$OUT"

"$CC" -std=c17 -O2 -Wall -Wextra -Werror \
    -I"$PXF/include" -I"$SHIM" \
    "$PXF/asset/nc_px_json.c" \
    "$PXF/asset/nc_px_asset.c" \
    "$PXF/asset/nc_px_name_dict.c" \
    "$PXF/asset/nc_px_layer.c" \
    "$PXF/asset/nc_px_show.c" \
    "$PXF/engine/nc_px_engine_tables.c" \
    "$PXF/engine/nc_px_engine_program.c" \
    "$T/pxMap_bench.c" -lm -o "$OUT/pxMap_bench"
"$OUT/pxMap_bench"
