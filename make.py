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

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / "tools"))

from ncbuild.cli import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
