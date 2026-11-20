#!/usr/bin/env bash
# sin_bench — 正弦計算的 host 基準（唔係 pass/fail 測試，係量數用；唔入 run_all.sh）
#
#   bash tools/hosttest/sin_bench/run.sh              完整（REP=120）
#   bash tools/hosttest/sin_bench/run.sh --quick      快（REP=20）
#   bash tools/hosttest/sin_bench/run.sh --json       機器可讀（存檔／比較用）
#   CC=clang bash tools/hosttest/sin_bench/run.sh
#
# ★ 這支會直接 link firmware 的 nc_px_engine_tables.c —— 量的係出貨中的
#   nc_px_engine_sin／sin_lin／sin_q12，唔係工具自己抄的一份。
#
# ⚠️ 數字係 host（x86-64），唔係 ESP32。用來看**相對**成本同精度；
#    實機請用 0xF063 SINBENCH（同目錄 dev_probe.py --sin）。
set -euo pipefail
cd "$(dirname "$0")/../../.."

CC=${CC:-cc}
PXF=firmware/components/sw/pixel
T=tools/hosttest/sin_bench
OUT=$T/build
mkdir -p "$OUT"

"$CC" -std=c17 -O2 -Wall -Wextra -Werror \
    -I"$PXF/include" -I"$T/include" \
    -o "$OUT/sin_bench" "$T/sin_bench.c" "$PXF/engine/nc_px_engine_tables.c" -lm
"$OUT/sin_bench" "$@" | tee "$OUT/out.txt"
