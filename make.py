#!/usr/bin/env python3
"""esp_Net-Core 建置入口。

    python3 make.py p4               # 建置
    python3 make.py doctor           # 環境健檢
    python3 make.py flash p4 --full
    python3 make.py boards

實作在 tools/ncbuild/；板子的真相在 ports/<chip>/<board>/board.json。
"""
import sys
from pathlib import Path

# Windows console 的預設編碼是 cp950：print("✔")／"✘"／"↔" 會直接
# UnicodeEncodeError（不是顯示問題，是整個命令 traceback 收場）。
# 只放寬 errors，不改 encoding —— 這樣中文仍然用 cp950 正常輸出。
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(errors="replace")

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / "tools"))

from ncbuild.cli import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
