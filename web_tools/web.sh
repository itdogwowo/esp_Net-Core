#!/usr/bin/env bash
# 一鍵啟動 ESP WebMaster（macOS／Linux）。
#
# 位置：repo 根 → web_tools/web.sh
#   repo/
#   ├─ make.py / build.sh / make.bat     建置（同網頁工具無關）
#   └─ web_tools/
#      ├─ settings.json                  ← 基礎數值（port／host／autoconnect…）
#      ├─ settings.local.json            ← 只係我部機（唔入 git，選用）
#      ├─ web.sh / WebMaster.command     macOS／Linux
#      ├─ web.ps1 / WebMaster.bat        Windows
#      └─ webmaster/                     ← 程式本體（run.py 喺呢度）
#
# 平台對照：
#   macOS / Linux → ./web_tools/web.sh    （本檔）
#   Windows       → web_tools\WebMaster.bat（雙擊）或 .\web_tools\web.ps1
#
#   ./web_tools/web.sh                    跟 settings.json（預設 port 8010＋開瀏覽器）
#   ./web_tools/web.sh --sim              用模擬裝置（唔使板）
#   ./web_tools/web.sh --port 8011        換 port（命令列永遠贏設定檔）
#   ./web_tools/web.sh --print-config     睇合併後的實際設定
#
# 設定值全部喺 web_tools/settings.json（同本檔同層）；本檔唔再自己寫死 port／--open。
set -euo pipefail
cd "$(dirname "$0")/webmaster"

if python3 -c "import fastapi, uvicorn, serial" >/dev/null 2>&1; then
    exec python3 run.py "$@"
else
    exec python3 launch.py "$@"
fi
