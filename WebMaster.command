#!/bin/bash
# macOS 雙擊啟動（Terminal 會自動開）。
cd "$(dirname "$0")" || exit 1
./web.sh "$@"
printf '\nWebMaster 已停止，按 Enter 關閉…'
read -r _
