#!/usr/bin/env bash
# pxLayer_test — DATA 平面第二～四層（map／modes／registry）的 host 測試
#
#   bash tools/hosttest/pxLayer_test/run.sh
#   CC=clang bash tools/hosttest/pxLayer_test/run.sh
set -euo pipefail
cd "$(dirname "$0")/../../.."

CC=${CC:-cc}
PXF=firmware/components/sw/pixel
T=tools/hosttest/pxLayer_test
OUT="$T/build"
mkdir -p "$OUT"

"$CC" -std=c17 -Wall -Wextra -Werror -O1 \
    -I"$PXF/include" -I"$T/include" \
    "$PXF/asset/nc_px_json.c" \
    "$PXF/asset/nc_px_layer.c" \
    "$PXF/asset/nc_px_asset.c" \
    "$PXF/asset/nc_px_asset_scan.c" \
    "$PXF/asset/nc_px_layer_scan.c" \
    "$T/pxLayer_test.c" \
    -o "$OUT/pxLayer_test"
"$OUT/pxLayer_test"
