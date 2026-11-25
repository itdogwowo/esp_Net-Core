#!/usr/bin/env python3
"""tenv —— 測試環境（每台機唔同）。

    python3 tools/tenv.py brief      # 每次開工先睇
    python3 tools/tenv.py plan -v    # 邊條 lane 跑得、閘門係咩
    python3 tools/tenv.py stamp      # 貼入測試紀錄的一行
    python3 tools/tenv.py init       # 第一次在這台機建立紀錄

實作在 tools/testenv/；協議見 docs/02_guides/test-environment.md。
"""
import sys
from pathlib import Path

# Windows console 的 cp950 遇到 ✔／✘／─ 會 UnicodeEncodeError（同 make.py 同因）。
# 只放寬 errors，不改 encoding —— 中文仍然用 cp950 正常輸出。
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(errors="replace")

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

from testenv.cli import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
