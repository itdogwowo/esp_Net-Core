#!/usr/bin/env python3
"""Windows 控制台編碼保護。

問題
----
Windows 的 Python 預設用**主控台代碼頁**（繁中系統 = `cp950`／Big5）輸出。
只要有一隻字唔喺 Big5 裡面（例如 `喺` \u55BA、`嘅`、`搵`），`print()` 就會
`UnicodeEncodeError` 直接收場——**唔係顯示亂碼，係整個程式崩潰**。
實測：`run.py --print-config` 印設定檔註解就中招（第 615 個字）。

呢個 repo 已有先例：`make.py` 頂部對 `sys.stdout` 做 `errors="replace"`。

做法
----
| 情況 | encoding | errors |
|---|---|---|
| 真主控台（tty） | 保持系統碼頁（cp950） | `replace` |
| 被 pipe／重定向到檔案 | **UTF-8** | `replace` |

- 主控台唔改 encoding：硬寫 utf-8 會令 cp950 主控台整版中文變亂碼。
  Big5 沒有的少數怪字變成 `?`，但程式繼續跑。
- 重定向去檔案（`WebMaster.bat > boot.log`）就用 UTF-8：任何編輯器／
  `git diff` 都讀得明（原本寫 cp950 會變亂碼）。

用法（每個 entry point 最先呼叫）
--------------------------------
    import console
    console.setup()
"""
from __future__ import annotations

import sys


def _isatty(stream) -> bool:
    try:
        return bool(stream.isatty())
    except (AttributeError, ValueError, OSError):
        return False


def setup() -> None:
    """設定 stdout／stderr 的編碼行為（唔會 raise）。"""
    for stream in (sys.stdout, sys.stderr):
        if stream is None:
            continue
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is None:          # 例如已被替換成 StringIO（測試／IDE）
            continue
        try:
            if _isatty(stream):
                # 唔改 encoding：硬改 utf-8 會令 cp950 主控台整版中文變亂碼
                reconfigure(errors="replace")
            else:
                reconfigure(encoding="utf-8", errors="replace")
        except (ValueError, OSError, LookupError):
            # 某些 stream（已關閉／管道）唔支援；唔應該因此啟動失敗
            pass


def safe(text: object) -> str:
    """把字串轉成「一定印得出」的版本（測試同 log 用）。"""
    enc = getattr(sys.stdout, "encoding", None) or "utf-8"
    try:
        return str(text).encode(enc, errors="replace").decode(enc, errors="replace")
    except (LookupError, UnicodeError):
        return str(text)
