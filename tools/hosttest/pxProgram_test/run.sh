#!/usr/bin/env bash
# pxProgram_test — DATA 平面 program interpreter 的 host 測試（唔需要 ESP-IDF／板子）。
#
#   bash tools/hosttest/pxProgram_test/run.sh
#   CC=clang bash tools/hosttest/pxProgram_test/run.sh
#
# 驗：段落表 → 預算值（compile）、逐幀取值、一幀的空間分布（step／spacing／offset／
#     speed／reverse）、等級 → 8.8 畫布（gain／gamma／顏色）、壞資產拒絕。
# 期望值全部由 MicroPython 版（mp_Net-Core/slave）跑出來，C 這邊唔可以自己解釋一次。
set -euo pipefail
cd "$(dirname "$0")/../../.."

CC=${CC:-cc}
PXF=firmware/components/sw/pixel
T=tools/hosttest/pxProgram_test
OUT="$T/build"
mkdir -p "$OUT"

"$CC" -std=c17 -Wall -Wextra -Werror -O1 \
    -I"$PXF/include" -I"$T/include" -I"$T" -I"tools/hosttest/pxEngine_test/include" \
    "$PXF/asset/nc_px_json.c" \
    "$PXF/asset/nc_px_asset.c" \
    "$PXF/asset/nc_px_name_dict.c" \
    "$PXF/engine/nc_px_engine_tables.c" \
    "$PXF/engine/nc_px_engine_program.c" \
    "$T/pxProgram_test.c" \
    -lm -o "$OUT/pxProgram_test"
"$OUT/pxProgram_test"
