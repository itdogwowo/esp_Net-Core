#!/bin/bash
# macOS 雙擊啟動（Terminal 會自動開）。
#
# 位置：repo 根 → web_tools/WebMaster.command（同「建置」用的 build.sh 分開）
# 平台對照：macOS 用本檔或 ./web_tools/web.sh；Windows 用 web_tools\WebMaster.bat。
#
# 冇參數 = 只開 server（port 8010）＋自動開瀏覽器
# 要模擬裝置：./web_tools/WebMaster.command --sim
cd "$(dirname "$0")" || exit 1
./web.sh "$@"
printf '\nWebMaster 已停止，按 Enter 關閉…'
read -r _
