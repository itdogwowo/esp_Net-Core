#!/usr/bin/env bash
# 板子工具（相容殼）—— 實作在 tools/ncbuild，等同 `python3 make.py ...`
#
#   tools/board.sh list                 插著的板子 + 支援哪些
#   tools/board.sh build  [板子] [--minimal]
#   tools/board.sh flash  [板子] [--port P] [--full|--erase]
#   tools/board.sh monitor [板子]
#   tools/board.sh ports                誰佔住了序列埠
#
# console 不再用 --console 切換：那是「這塊板子的事」，
# 寫在 ports/<chip>/<board>/board.json 的 "console" 欄位。
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec python3 "$ROOT/make.py" "$@"
