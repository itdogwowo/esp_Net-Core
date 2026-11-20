#!/usr/bin/env bash
# sin_bench — 正弦計算的 host 基準（唔係 pass/fail 測試，係量數用；唔入 run_all.sh）
#
#   bash tools/hosttest/sin_bench/run.sh
#   CC=clang bash tools/hosttest/sin_bench/run.sh
#
# ⚠️ 數字係 host（x86-64），唔係 ESP32。用來看**相對**成本同精度；
#    上板請用固件 0xF062（nc_px_bench）。
set -euo pipefail
cd "$(dirname "$0")/../../.."

CC=${CC:-cc}
OUT=tools/hosttest/sin_bench/build
mkdir -p "$OUT"

"$CC" -std=c17 -O2 -Wall -Wextra -o "$OUT/sin_bench" tools/hosttest/sin_bench/sin_bench.c -lm
"$OUT/sin_bench" | tee "$OUT/out.txt"
