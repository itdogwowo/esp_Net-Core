#!/usr/bin/env bash
# verify_vs_mp.sh — 交叉驗證：C program interpreter ↔ MicroPython PixelMathMethod
#
# 兩邊用**同一組 program**（F=10、l_max=4095、l_lim=100、phi=0、end=100、pulse=2047）
# 各跑 40,000 步，逐值計 FNV-1a；checksum 一致 = 兩份實作語義相同。
#
#   bash tools/hosttest/program_bench/verify_vs_mp.sh
#   MP_REPO=/path/to/mp_Net-Core bash tools/hosttest/program_bench/verify_vs_mp.sh
set -uo pipefail
cd "$(dirname "$0")/../../.."
MP_DIR=${MP_REPO:-../mp_Net-Core}

if [ ! -d "$MP_DIR" ]; then
    echo "✘ 搵唔到 mp repo：$MP_DIR（用 MP_REPO=... 指定）" >&2
    exit 2
fi

c_out=$(bash tools/hosttest/program_bench/run.sh) || { echo "C bench 失敗" >&2; exit 1; }
mp_out=$(cd "$MP_DIR" && python3 -B test/pixel/bench_wave_segments.py 40000) || {
    echo "MP bench 失敗" >&2; exit 1; }

# 抽出「型別 checksum」（跳過 compile 行）
c_chk=$(printf '%s\n' "$c_out" | awk '/chk=/{for(i=1;i<=NF;i++) if($i ~ /^chk=/){print $1, substr($i,5); break}}' | sort)
mp_chk=$(printf '%s\n' "$mp_out" | grep -o '^[a-z_]* .*chk=[0-9A-F]*' | awk '{print $1, $NF}' | sed 's/chk=//' | sort)

echo "── C（host x86）────────────────"
printf '%s\n' "$c_chk"
echo "── MicroPython（CPython）───────"
printf '%s\n' "$mp_chk"
echo ""

if [ -z "$c_chk" ]; then
    echo "✘ 抽唔到 C 的 checksum（bench 輸出格式變了？）" >&2
    exit 1
fi

fail=0
while read -r t h; do
    [ -z "$t" ] && continue
    mh=$(printf '%s\n' "$mp_chk" | awk -v t="$t" '$1==t{print $2}')
    if [ "$h" = "$mh" ]; then
        printf '  ✅ %-16s %s\n' "$t" "$h"
    else
        printf '  ❌ %-16s C=%s MP=%s\n' "$t" "$h" "${mh:-（冇）}"
        fail=$((fail + 1))
    fi
done <<< "$c_chk"

if [ "$fail" -ne 0 ]; then
    echo "✘ $fail 個型別唔一致"
    exit 1
fi
echo "✓ 六個型別、40,000 步逐值一致（C 與 MicroPython 兩份獨立實作）"
