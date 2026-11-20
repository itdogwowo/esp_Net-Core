#!/usr/bin/env bash
# pxScan_test — map／modes 目錄掃描＋registry（host）
set -euo pipefail
cd "$(dirname "$0")/../../.."

CC=${CC:-cc}
PXF=firmware/components/sw/pixel
T=tools/hosttest/pxScan_test
OUT="$T/build"
SHIM=tools/hosttest/pxLayer_test/include
mkdir -p "$OUT"

"$CC" -std=c17 -Wall -Wextra -Werror -O1 \
    -I"$PXF/include" -I"$SHIM" \
    "$PXF/asset/nc_px_json.c" \
    "$PXF/asset/nc_px_asset.c" \
    "$PXF/asset/nc_px_asset_scan.c" \
    "$PXF/asset/nc_px_layer.c" \
    "$PXF/asset/nc_px_layer_scan.c" \
    "$T/pxScan_test.c" -o "$OUT/pxScan_test"
"$OUT/pxScan_test"
