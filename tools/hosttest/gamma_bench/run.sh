#!/usr/bin/env bash
# gamma_bench — 「有 gamma 表」同「冇 gamma」的分別（host；唔需要硬件）。
#
#   bash tools/hosttest/gamma_bench/run.sh               建置＋跑＋對 golden
#   bash tools/hosttest/gamma_bench/run.sh --update      重影 golden（曲線真的變了才用）
#   bash tools/hosttest/gamma_bench/run.sh --csv /tmp/gamma.csv   另外存 CSV 畫圖
#
# ★ 直接 link 出貨中的 nc_px_engine_tables.c，量嘅係同一張表。
# ⚠️ 數字係 host（x86-64）；**唔係**板上時間。板上 gamma 成本見
#    docs/00_pm/effect-perf.md §3.2（151 ns/值）或 0xF065 的 eff.gam_ns。
set -euo pipefail
cd "$(dirname "$0")/../../.."

CC=${CC:-cc}
PXF=firmware/components/sw/pixel
T=tools/hosttest/gamma_bench
OUT=$T/build
mkdir -p "$OUT"

"$CC" -std=c17 -O2 -Wall -Wextra -Werror \
    -I"$PXF/include" -I"$T/include" \
    -o "$OUT/gamma_bench" "$T/gamma_bench.c" "$PXF/engine/nc_px_engine_tables.c" -lm

args=()
for a in "$@"; do
    if [ "$a" != "--update" ]; then
        args+=("$a")
    fi
done
"$OUT/gamma_bench" ${args[@]+"${args[@]}"} | tee "$OUT/out.txt"

if [ "${1:-}" = "--update" ] || [ ! -f "$T/golden.txt" ]; then
    cp "$OUT/out.txt" "$T/golden.txt"
    echo "（golden.txt 已更新）"
    exit 0
fi

if diff -u "$T/golden.txt" "$OUT/out.txt" > "$OUT/golden.diff"; then
    echo "gamma_bench OK（golden 一致）"
else
    echo "✘ gamma_bench：曲線同 golden 唔一樣"
    cat "$OUT/golden.diff"
    exit 1
fi
