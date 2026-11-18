#!/usr/bin/env bash
# Linux／通用一鍵啟動（等同 web_tools/web.sh，用絕對路徑叫佢就唔怕 cd 過）。
#
# 平台對照：Linux 用本檔或 ./web_tools/web.sh；macOS 用 WebMaster.command；
#           Windows 用 web_tools\WebMaster.bat。
#
#   ./web_tools/run_web.sh                開 server（預設 port 8010）＋自動開瀏覽器
#   ./web_tools/run_web.sh --sim          模擬裝置（唔使板）
#   ./web_tools/run_web.sh --serial /dev/ttyACM0 --port 8011
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$ROOT/web.sh" "$@"
