#!/usr/bin/env bash
# 板子切換與燒錄工具
#
#   tools/board.sh list                 列出連著的 ESP 板子（型號 + 埠）
#   tools/board.sh build  [s3|p4] [--minimal] [--console jtag|otg]
#   tools/board.sh flash  [s3|p4] [--minimal] [--console jtag|otg]
#   tools/board.sh monitor [s3|p4]
#   tools/board.sh ports                誰佔住了序列埠
#
# console：
#   jtag = USB-Serial/JTAG（預設，S3 板上的 "USB" 埠）
#   otg  = 走 TinyUSB CDC（P4 只有一條線時用）
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# 每塊板子可綁自己的 IDF（P4 目前與 S3 同用 6.0.3；必要時 IDF_P4=... 覆寫）
IDF_S3="${IDF_S3:-$HOME/esp/esp-idf}"
IDF_P4="${IDF_P4:-$HOME/esp/esp-idf}"
export IDF_PATH="${IDF_PATH:-$IDF_S3}"
# shellcheck disable=SC1091
source "$IDF_PATH/export.sh" >/dev/null 2>&1

PORTS=(
  "s3:ports/S3/ESP32-S3-N16R8:esp32s3:$IDF_S3"
  "p4:ports/P4/ESP32-P4-ETH:esp32p4:$IDF_P4"
)

board_port_dir() {
  local want="$1"
  for entry in "${PORTS[@]}"; do
    IFS=: read -r name dir target idf <<<"$entry"
    if [[ "$name" == "$want" ]]; then echo "$dir $target $idf"; return 0; fi
  done
  echo "未知板子：$want（可用：s3 / p4）" >&2
  return 1
}

list_ports() { ls /dev/cu.usbmodem* 2>/dev/null || true; }

detect_board() {
  # 依序試每個埠，問 esptool 這是哪顆晶片
  for p in $(list_ports); do
    local chip
    chip=$(python -m esptool -p "$p" --before no-reset chip-id 2>/dev/null \
           | grep -oE "ESP32-(S3|P4)" | head -1 || true)
    case "$chip" in
      ESP32-P4) echo "p4:$p"; return 0 ;;
      ESP32-S3) echo "s3:$p"; return 0 ;;
    esac
  done
  return 1
}

who_holds() {
  for p in $(list_ports); do
    local holder
    holder=$(lsof "$p" 2>/dev/null | awk 'NR==2{print $1" (pid "$2")"}')
    printf '%-32s %s\n' "$p" "${holder:-（空閒）}"
  done
}

cmd_list() {
  echo "序列埠："
  who_holds
  echo
  local found
  if found=$(detect_board); then
    echo "偵測到：${found%%:*} → ${found##*:}"
  else
    echo "偵測不到可辨識的板子（可能被其他程式佔住，或正在跑 app）"
  fi
}

resolve_target() {
  local want="${1:-auto}"
  if [[ "$want" == "auto" ]]; then
    local found
    if found=$(detect_board); then want="${found%%:*}"; else want="s3"; fi
  fi
  board_port_dir "$want"
}

do_build() {
  local want="${1:-auto}" minimal="${2:-0}" console="${3:-jtag}"
  read -r dir target idf <<<"$(resolve_target "$want")"
  [[ -n "${dir:-}" ]] || { echo "找不到板子設定" >&2; exit 1; }
  local args=(-DPORT="$(basename "$(dirname "$dir")")/$(basename "$dir")")
  [[ "$minimal" == "1" ]] && args+=(-DNETCORE_MINIMAL=1)
  [[ "$console" == "otg" ]] && args+=(-DNETCORE_CONSOLE_OTG=1)
  local extra=""
  [[ "$minimal" == "1" ]] && extra+=", minimal"
  [[ "$console" != "jtag" ]] && extra+=", console=$console"
  echo "→ 建置 ${dir}（target=${target}, IDF=$(basename "${idf}")${extra}）"
  (
    export IDF_PATH="$idf"
    # shellcheck disable=SC1090
    source "$IDF_PATH/export.sh" >/dev/null 2>&1
    cd "$ROOT"
    # ★ 目標晶片只由這裡決定：sdkconfig 記的是「上一次」的 target，
    #   只換 -DPORT 會拿 A 板的設定去建 B 板（實測：build s3 其實建出了 P4 image）。
    #   換 target 等於重設 sdkconfig 與 build/ —— 所有可調值都必須在 sdkconfig.defaults 裡。
    if [[ -f sdkconfig ]] && ! grep -q "^CONFIG_IDF_TARGET=\"$target\"" sdkconfig; then
      local now
      now=$(sed -n 's/^CONFIG_IDF_TARGET="\(.*\)"/\1/p' sdkconfig | head -1)
      echo "→ 目標由 ${now:-?} 改成 ${target}：重設 sdkconfig（來源 = sdkconfig.defaults + 板子覆蓋）"
      idf.py -B build "${args[@]}" -DIDF_TARGET="$target" set-target "$target" >/dev/null
    fi
    idf.py -B build "${args[@]}" -DIDF_TARGET="$target" build
  )
}

do_flash() {
  local want="${1:-auto}" minimal="${2:-0}" console="${3:-jtag}"
  local port
  if port=$(list_ports | head -1) && [[ -n "$port" ]]; then :; else
    echo "找不到序列埠" >&2; exit 1
  fi
  # 清掉會佔住埠的殘留 monitor
  pkill -f "esp_idf_monitor.*$port" 2>/dev/null || true
  pkill -f "idf_monitor.py.*$port" 2>/dev/null || true
  sleep 0.5
  do_build "$want" "$minimal" "$console"
  read -r dir target idf <<<"$(resolve_target "$want")"
  local args=(-DPORT="$(basename "$(dirname "$dir")")/$(basename "$dir")")
  [[ "$minimal" == "1" ]] && args+=(-DNETCORE_MINIMAL=1)
  [[ "$console" == "otg" ]] && args+=(-DNETCORE_CONSOLE_OTG=1)
  echo "→ 燒錄到 ${port}（IDF=$(basename "${idf}")）"
  (
    export IDF_PATH="$idf"
    # shellcheck disable=SC1090
    source "$IDF_PATH/export.sh" >/dev/null 2>&1
    cd "$ROOT" && idf.py -B build "${args[@]}" -DIDF_TARGET="$target" -p "$port" flash
  )
}

do_monitor() {
  local port
  port=$(list_ports | head -1)
  [[ -n "$port" ]] || { echo "找不到序列埠" >&2; exit 1; }
  echo "→ ${port}（Ctrl+] 離開）"
  ( cd "$ROOT" && idf.py -p "$port" monitor )
}

main() {
  local cmd="${1:-list}"; shift || true
  case "$cmd" in
    list)    cmd_list ;;
    ports)   who_holds ;;
    build)   do_build "${1:-auto}" "${2:-0}" "${3:-jtag}" ;;
    flash)   do_flash "${1:-auto}" "${2:-0}" "${3:-jtag}" ;;
    monitor) do_monitor ;;
    *) sed -n '2,16p' "${BASH_SOURCE[0]}"; exit 1 ;;
  esac
}

main "$@"
