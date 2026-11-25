#!/usr/bin/env bash
# 夜間長跑：等板 → OTA 安全映像 → 4 小時 FPS/漂移測試。
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
PORT=""
for i in $(seq 1 720); do          # 最多等 2 小時
    for p in /dev/cu.usbmodem*; do
        [ -e "$p" ] && PORT="$p" && break
    done
    [ -n "$PORT" ] && break
    sleep 10
done
if [ -z "$PORT" ]; then
    echo "沒有 port（板未連上）"
    exit 2
fi
echo "板：$PORT"
python3 tools/ota.py --abort --port "$PORT" >/dev/null 2>&1 || true
for i in 1 2 3 4 5; do
    if python3 tools/ota.py artifacts/ESP32-S3-Zero.bin --port "$PORT" \
            --chunk 1024 --window 1 2>&1 | grep -q "END ok"; then
        echo "OTA ok"
        break
    fi
    echo "OTA 重試 $i…"
    sleep 5
done
sleep 10
mkdir -p artifacts
python3 tools/fps_longrun.py --port "$PORT" --hours 4 --interval 60 \
    --csv artifacts/fps_longrun.csv
