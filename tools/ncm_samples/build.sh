#!/usr/bin/env bash
# 造外部任務樣本：WAT → wasm → .nct（manifest + meta 一起打包）。
#
# 需要 wat2wasm（brew install wabt／apt install wabt）。
# 產物在 tools/ncm_samples/out/：
#   hello_task.wasm  純 wasm payload
#   hello_task.nct   上板用的容器
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT="$HERE/out"
mkdir -p "$OUT"

# hello_task：列印＋讀／反轉 GPIO
wat2wasm "$HERE/hello_task.wat" -o "$OUT/hello_task.wasm"
python3 "$ROOT/tools/ncm.py" pack \
    --payload "$OUT/hello_task.wasm" \
    --out "$OUT/hello_task.nct" \
    --kind task --fps 1 --core -1 --layer 4 --prio 3 \
    --mem 65536 --stack 4096 --budget-us 20000 \
    --meta "$HERE/hello_task.meta.json"
python3 "$ROOT/tools/ncm.py" dump "$OUT/hello_task.nct"

# bench_task：同 0xF050 CPU_BURN 一樣的 LCG 迴圈（wasm 直譯 vs native C 對照）
wat2wasm "$HERE/bench_task.wat" -o "$OUT/bench_task.wasm"
python3 "$ROOT/tools/ncm.py" pack \
    --payload "$OUT/bench_task.wasm" \
    --out "$OUT/bench_task.nct" \
    --kind task --fps 1 --core -1 --layer 4 --prio 3 \
    --mem 65536 --stack 4096 --budget-us 100000 \
    --meta "$HERE/bench_task.meta.json"
python3 "$ROOT/tools/ncm.py" dump "$OUT/bench_task.nct"

cat <<TXT

上板（S3 原生 USB，MSC 隨身碟）：
  cp "$OUT/hello_task.nct" "/Volumes/NC-<cid>/tasks/"
  tools/nc4.py --port /dev/cu.usbmodem* reboot
  然後看 CDC log：每秒一行 "ncm.wamr: [hello_task.nct] tick pin21 read=… toggle=…"
TXT
