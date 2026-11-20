#!/usr/bin/env bash
# program_bench — C 側 program interpreter 的 10,000 步量測（host，唔需要 IDF／板）
#
#   bash tools/hosttest/program_bench/run.sh
#   CC=clang bash tools/hosttest/program_bench/run.sh
#
# 量 nc_px_engine_program_value()（板上真正跑的 C 實作）；
# 同 mp 的 test/pixel/bench_wave_segments.py 同一種量法（10,000 步）。
set -euo pipefail
cd "$(dirname "$0")/../../.."

CC=${CC:-cc}
PXF=firmware/components/sw/pixel
T=tools/hosttest/program_bench
SHIM=tools/hosttest/pxProgram_test/include
OUT="$T/build"
mkdir -p "$OUT"

"$CC" -std=c17 -O2 -Wall -Wextra -Werror \
    -I"$PXF/include" -I"$SHIM" \
    "$PXF/engine/nc_px_engine_tables.c" \
    "$PXF/engine/nc_px_engine_program.c" \
    "$T/program_bench.c" -lm -o "$OUT/program_bench"
"$OUT/program_bench"
