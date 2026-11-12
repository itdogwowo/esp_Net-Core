#!/usr/bin/env python3
"""一鍵啟動：建 venv → 裝 deps → run.py（參數原樣轉交）。

    python launch.py --sim
    python launch.py --serial COM13 --open
"""
from __future__ import annotations

import os
import subprocess
import sys
import venv
from pathlib import Path

BASE = Path(__file__).resolve().parent
VENV = BASE / ".venv"


def venv_python() -> Path:
    return VENV / ("Scripts/python.exe" if os.name == "nt" else "bin/python")


def ensure_venv() -> Path:
    py = venv_python()
    if not py.exists():
        print("[esp_webmaster] 建立 venv…")
        venv.EnvBuilder(with_pip=True).create(VENV)
    req = BASE / "requirements.txt"
    if req.exists():
        print("[esp_webmaster] 安裝 dependencies…")
        subprocess.check_call([str(py), "-m", "pip", "install", "-q", "-r", str(req)])
    return py


def main() -> int:
    py = ensure_venv()
    args = [str(py), str(BASE / "run.py"), *sys.argv[1:]]
    print("[esp_webmaster]", " ".join(args))
    return subprocess.call(args)


if __name__ == "__main__":
    raise SystemExit(main())
