#!/usr/bin/env bash
# ══════════════════════════════════════════════════════════════════════════
#  ESP Net-Core — 建置腳本（macOS／Linux；等同 python3 make.py）
#
#  平台對照：macOS／Linux 用本檔；Windows 用 make.bat。
#
#  用法（同 make.py 完全一樣）：
#    ./build.sh doctor                    環境健檢
#    ./build.sh boards                    列出所有板
#    ./build.sh build s3                  建置（S3）
#    ./build.sh build p4 --tests          建置（P4 + 測試）
#    ./build.sh build --all               全部板
#    ./build.sh flash s3                  燒錄
#    ./build.sh check                     檢查 config／resources
#
#  實作喺 tools/ncbuild/；板子的真相喺 ports/<chip>/<board>/board.json。
#  ESP-IDF 要先 export：. ~/esp/esp-idf/export.sh
# ══════════════════════════════════════════════════════════════════════════
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

PY="${PYTHON:-python3}"
if ! command -v "$PY" >/dev/null 2>&1; then
    echo "[build] 搵唔到 python3；請裝 Python 3.9+ 或設 PYTHON=…" >&2
    exit 1
fi
exec "$PY" "$ROOT/make.py" "$@"
