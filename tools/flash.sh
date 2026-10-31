#!/usr/bin/env bash
# esp_Net-Core — 一鍵燒錄
#
# 為什麼需要這支：
#   firmware 跑起來之後 TinyUSB 會佔住 USB PHY，esptool 進不了 ROM download mode。
#   板子有逃生門（CDC 收 "DFU" 或 BOOT 長按 3 秒），這支腳本負責：
#     1. 等 ROM 埠出現（你去按 BOOT 3 秒）
#     2. 燒錄
#     3. 等 app 埠回來，順便印幾行心跳
#
# 用法：
#   source ~/esp/esp-idf/export.sh
#   tools/flash.sh              # 只燒 app（保留碟上的使用者資料）
#   tools/flash.sh --full       # 連分割表/bootloader 一起燒（不動碟）
#   tools/flash.sh --erase      # 全清再燒（碟會回到出廠預置）
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_DIR"

: "${IDF_PATH:?請先 source \$IDF_PATH/export.sh}"
command -v idf.py >/dev/null || { echo "找不到 idf.py"; exit 1; }

MODE="app"
case "${1:-}" in
    --full)  MODE="full" ;;
    --erase) MODE="erase" ;;
    "")      ;;
    *) echo "未知參數: $1"; exit 1 ;;
esac

# ROM 的 USB-Serial-JTAG 會回答 esptool；app 的 TinyUSB CDC 不會。
find_rom_port() {
    local p
    for p in /dev/cu.usbmodem*; do
        [ -e "$p" ] || continue
        if python -m esptool -c esp32s3 -p "$p" --before no-reset read-mac \
                >/dev/null 2>&1; then
            echo "$p"
            return 0
        fi
    done
    return 1
}

find_app_port() {
    local p
    for p in /dev/cu.usbmodem*; do
        [ -e "$p" ] || continue
        # app 埠的特徵：serial 是晶片 MAC（D8:85:AC:…），ROM 埠是 1401 之類的短名
        case "$(basename "$p")" in
            *1401) continue ;;
        esac
        echo "$p"
        return 0
    done
    return 1
}

echo "→ 找 ROM 埠…"
if ! PORT="$(find_rom_port)"; then
    echo
    echo "  找不到 ROM 埠。板子跑著 TinyUSB，需要進 download mode："
    echo "    • 按住 BOOT 不放約 5 秒（3 秒觸發重啟，手不要放開，ROM 才看得到 GPIO0=0）"
    echo "    • 或 printf 'DFU' 到 app 的 CDC 埠（需要 host 端 DTR 已建立）"
    echo
    for _ in $(seq 1 30); do
        sleep 2
        if PORT="$(find_rom_port)"; then break; fi
        printf '.'
    done
    echo
    [ -n "${PORT:-}" ] || { echo "  等不到 ROM 埠，放棄"; exit 1; }
fi
echo "  ROM 埠: $PORT"

case "$MODE" in
    app)
        echo "→ app-flash（不動 data 分區）"
        idf.py -p "$PORT" app-flash
        ;;
    full)
        echo "→ flash（bootloader + 分割表 + app）"
        idf.py -p "$PORT" flash
        ;;
    erase)
        echo "→ erase-flash 後全燒"
        idf.py -p "$PORT" erase-flash
        idf.py -p "$PORT" flash
        ;;
esac

# idf.py flash 的 hard-reset 對 USB-JTAG 無效（板子會留在 ROM）。
# 用 watchdog reset 把它踢回 app。
echo "→ 重啟進 app"
python -m esptool -c esp32s3 -p "$PORT" --after watchdog-reset read-mac \
    >/dev/null 2>&1 || true

echo "→ 等 app 埠回來…"
sleep 3
APP_PORT=""
for _ in $(seq 1 15); do
    if APP_PORT="$(find_app_port)"; then break; fi
    sleep 1
done

if [ -z "$APP_PORT" ]; then
    echo "  ⚠️ app 埠沒回來。看門狗/初始化可能失敗，用 idf.py monitor 查。"
    exit 0
fi

echo "  app 埠: $APP_PORT"
echo "→ 心跳（3 秒）:"
python - "$APP_PORT" <<'PY'
import serial, sys, time
try:
    s = serial.Serial(sys.argv[1], 115200, timeout=0.2)
except Exception as e:
    print("  開不了埠:", e); raise SystemExit
d = b''; t = time.time()
while time.time() - t < 3:
    d += s.read(2048)
s.close()
for line in d.decode('utf-8', 'replace').splitlines():
    if 'netcore' in line:
        print("  " + line)
PY

echo "完成。碟上的內容: open $APP_PORT 或用 idf.py -p $APP_PORT monitor"
