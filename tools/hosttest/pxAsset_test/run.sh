#!/usr/bin/env bash
# pxAsset_test — DATA 平面資產解析的 host 測試（唔需要 ESP-IDF、FAT、板子）。
#
#   bash tools/hosttest/pxAsset_test/run.sh
#   CC=clang bash tools/hosttest/pxAsset_test/run.sh
#
# 驗：JSON → struct（批次／單檔／檔名補名）、壞檔／未知段拒絕、撞名、上限、狀態 JSON，
#     以及掃目錄（effects.json 先行 → 檔名排序 → .nce）。
# 注意：這個測試會建／刪 ./pxAsset_tmp/ 目錄（跑完自己清）。
set -euo pipefail
cd "$(dirname "$0")/../../.."

CC=${CC:-cc}
PXF=firmware/components/sw/pixel
T=tools/hosttest/pxAsset_test
OUT="$T/build"
mkdir -p "$OUT"

"$CC" -std=c17 -Wall -Wextra -Werror -O1 \
    -I"$PXF/include" -I"$T/include" -I"$T" \
    "$PXF/asset/nc_px_json.c" \
    "$PXF/asset/nc_px_asset.c" \
    "$PXF/asset/nc_px_name_dict.c" \
    "$PXF/asset/nc_px_asset_scan.c" \
    "$T/pxAsset_test.c" \
    -o "$OUT/pxAsset_test"
"$OUT/pxAsset_test"
