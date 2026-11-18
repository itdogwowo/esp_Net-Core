#!/usr/bin/env python3
"""一鍵啟動：建 venv → 裝 deps → run.py（參數原樣轉交）。

    python launch.py                     # 跟 settings.json
    python launch.py --sim
    python launch.py --serial COM13
    python launch.py --print-config      # 唔使 start server，只睇設定

呢個檔只負責「環境」；**所有基礎數值（port／host／連邊個裝置）喺 run.py 讀
settings.json**。兩邊都寫死預設值就一定會漂移——所以呢度一個預設都唔寫。
"""
from __future__ import annotations

import os
import subprocess
import sys
import venv
from pathlib import Path

import console

# ★ Windows 主控台 cp950：印中文可能 UnicodeEncodeError，先放寬 errors
console.setup()

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
        # --no-cache-dir：某些機器的 pip wheel 快取目錄被 ACL 鎖住，寫快取會令 install 失敗
        subprocess.check_call([str(py), "-m", "pip", "install", "-q", "--no-cache-dir",
                               "--disable-pip-version-check", "-r", str(req)])
    return py


def main() -> int:
    py = ensure_venv()
    args = [str(py), str(BASE / "run.py"), *sys.argv[1:]]
    print("[esp_webmaster]", " ".join(args))
    return subprocess.call(args)


if __name__ == "__main__":
    raise SystemExit(main())
