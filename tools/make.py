#!/usr/bin/env python3
"""tools/make.py —— 從 tools/ 目錄也能跑（實際入口在 repo 根的 make.py）。"""
import runpy
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.argv[0] = str(ROOT / "make.py")
runpy.run_path(str(ROOT / "make.py"), run_name="__main__")
