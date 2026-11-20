#!/usr/bin/env bash
# fx_test — 效果引擎 host 測試（不需要 ESP-IDF、不需要燈板）
#
#   bash tools/hosttest/fx_test/run.sh            建置、跑、對 golden.txt
#   bash tools/hosttest/fx_test/run.sh --update   重影 golden（行為真的變了才用）
#   CC=clang bash tools/hosttest/fx_test/run.sh
#
# golden.txt = 同一支測試在「重構前」影低的 payload hash。
# 改引擎／效果之後如果 hash 變了，先問「這個改動應該改變畫面嗎？」再決定是修 code 還是 --update。
set -euo pipefail
cd "$(dirname "$0")/../../.."

CC=${CC:-cc}
PXF=firmware/components/sw/pixel
FXT=tools/hosttest/fx_test
OUT="$FXT/build"
mkdir -p "$OUT"

FXE=firmware/components/effects/pixel_effects
SRCS=("$PXF/engine/nc_px_engine_core.c" "$PXF/engine/nc_px_engine_tables.c"
      "$FXE/nc_effect_zone1.c" "$FXE/nc_effect_breath.c" "$FXE/nc_effect_wave.c"
      "$FXE/nc_effect_solid.c" "$FXE/nc_effect_ramp.c" "$FXE/nc_effect_table.c")
INCS=(-I"$PXF/include" -I"$FXE/include")

"$CC" -std=c17 -D_DEFAULT_SOURCE -Wall -Wextra -Wno-unused-parameter \
    "${INCS[@]}" -I"$FXT/include" -I"$FXT" \
    -o "$OUT/fx_test" "${SRCS[@]}" "$FXT/fx_test.c" -lm

"$OUT/fx_test" | tee "$OUT/out.txt"

if [ "${1:-}" = "--update" ] || [ ! -f "$FXT/golden.txt" ]; then
    cp "$OUT/out.txt" "$FXT/golden.txt"
    echo "（golden.txt 已更新）"
    exit 0
fi

if diff -u "$FXT/golden.txt" "$OUT/out.txt" > "$OUT/golden.diff"; then
    echo "fx_test OK（golden 一致）"
else
    echo "✘ fx_test：畫面同 golden 唔一樣"
    cat "$OUT/golden.diff"
    exit 1
fi
