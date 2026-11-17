#!/usr/bin/env bash
# 一鍵啟動 ESP WebMaster（repo 根）。
#
#   ./web.sh                    開 server（預設 port 8010）＋自動開瀏覽器
#   ./web.sh --sim              用模擬裝置（唔使板）
#   ./web.sh --serial /dev/cu.usbmodemXXXX --port 8011
#
# 系統 python 已經有 fastapi/uvicorn/pyserial → 直接跑；
# 否則用 launch.py 自動建 webmaster/.venv 並裝 requirements.txt。
set -euo pipefail
cd "$(dirname "$0")/webmaster"

if python3 -c "import fastapi, uvicorn, serial" >/dev/null 2>&1; then
    exec python3 run.py --open "$@"
else
    exec python3 launch.py --open "$@"
fi
