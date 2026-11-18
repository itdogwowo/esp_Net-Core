#!/usr/bin/env python3
"""NC4 文字檔格式化（server 側的唯一實作）。

為咩放呢度而唔係純喺瀏覽器？
- **一份真相**：`web_tools/webmaster/static/app.js` 嘅「格式化」同呢度要同意；有 host test
  鎖住（tests/test_format.py）。UI 會用 server 格式化的結果為準。
- **唔整爛編號**：config 檔案由人睇，`{"a":1}` 同 `{"a": 1}` 都合法，
  但重寫成 2 空格＋尾端換行可以令 diff 乾淨。
- **唔吞資料**：parse 失敗直接報錯（帶行／欄），**唔會**回原文——唔好俾 UI
  以為「格式化成功」而覆蓋壞內容。

支援：`.json`（嚴格 JSON）同 `.jsonc`／`.ndjson` 之類暫不支援；其他副檔名一律
`unsupported`，由 UI 提示（唔會靜靜改壞）。
"""
from __future__ import annotations

import json

SUPPORTED_EXTS = (".json",)

# 裝置 FAT 寫入係 4 KB chunk：格式化後唔應該急升（防呆上限）
MAX_INPUT = 1024 * 1024


class FormatError(ValueError):
    def __init__(self, msg: str, line: int = 0, col: int = 0):
        super().__init__(msg)
        self.line = line
        self.col = col


def supports(path: str) -> bool:
    return str(path).lower().endswith(SUPPORTED_EXTS)


def format_text(path: str, text: str, indent: int = 2, sort_keys: bool = False) -> dict:
    """格式化 JSON 文字。回 `{changed, text, bytes_in, bytes_out}`。

    raise FormatError：副檔名唔支援／太大／JSON 壞（帶 line、col，1-based）。
    """
    if not supports(path):
        raise FormatError("只支援 JSON")
    if len(text.encode("utf-8")) > MAX_INPUT:
        raise FormatError("檔案太大（>1 MiB），唔喺瀏覽器格式化")
    try:
        value = json.loads(text)
    except json.JSONDecodeError as e:
        raise FormatError(e.msg, line=e.lineno, col=e.colno) from None
    out = json.dumps(value, indent=indent, ensure_ascii=False, sort_keys=sort_keys) + "\n"
    return {
        "changed": out != text,
        "text": out,
        "bytes_in": len(text.encode("utf-8")),
        "bytes_out": len(out.encode("utf-8")),
    }
