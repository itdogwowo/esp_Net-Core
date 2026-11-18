"""Windows 主控台編碼（cp950）回歸測試。

背景（實際踩過）：繁中 Windows 的 Python 用 cp950 輸出，`print()` 遇到 Big5 冇的字
（`喺` U+55BA、`嘅`、`搵`）會 `UnicodeEncodeError`，**整個程式 traceback 收場**
——`run.py --print-config` 就係咁死（設定檔 `_comment` 裡有「喺」）。

同時鎖住：
- `--print-config` / `/api/config` 唔應該把 `_comment` 當成設定值；
- 重定向到檔案時寫 UTF-8（日誌可讀）。
"""
import io
import json
import sys

import pytest

import console
import settings as cfg


class _TTYStream(io.TextIOWrapper):
    """假裝是真主控台（`isatty()` 回 True），用嚟模擬 cp950 終端。"""

    def isatty(self) -> bool:      # noqa: D102
        return True


def _stream(encoding="cp950", tty=True):
    buf = io.BytesIO()
    cls = _TTYStream if tty else io.TextIOWrapper
    return buf, cls(buf, encoding=encoding, errors="strict", newline="")


# ── cp950 確實會爆（證明下面嘅修法有意義）────────────────

def test_cp950_stream_would_crash_without_setup():
    _buf, stream = _stream()
    with pytest.raises(UnicodeEncodeError):
        stream.write("喺")
        stream.flush()


# ── 真主控台：保持 cp950，怪字變 ?，正常中文照出 ───────────

def test_console_setup_makes_cp950_terminal_safe():
    buf, stream = _stream(tty=True)
    old = sys.stdout
    sys.stdout = stream
    try:
        console.setup()
        assert stream.encoding.lower().replace("-", "") == "cp950"   # encoding 冇被改
        print("喺嘅搵")            # 三隻都係 Big5 冇的字
        print("正常中文：晶片")     # 這句要原樣出到
        stream.flush()
    finally:
        sys.stdout = old
    text = buf.getvalue().decode("cp950", errors="replace")
    assert "?" in text             # 怪字變 ?
    assert "晶片" in text           # 正常字照樣出（唔會整版變亂碼）


# ── 重定向／pipe：改用 UTF-8（日誌可讀）──────────────────

def test_console_setup_uses_utf8_when_not_a_tty():
    buf, stream = _stream(tty=False)
    old = sys.stdout
    sys.stdout = stream
    try:
        console.setup()
        assert stream.encoding.lower().replace("-", "") == "utf8"
        print("喺嘅搵 正常中文")
        stream.flush()
    finally:
        sys.stdout = old
    text = buf.getvalue().decode("utf-8")
    assert "喺嘅搵" in text and "正常中文" in text


# ── 容錯 ────────────────────────────────────────────────

def test_console_setup_is_idempotent_and_tolerates_plain_streams():
    console.setup()
    console.setup()               # 叫兩次唔應該爆
    old = sys.stdout
    sys.stdout = io.StringIO()    # 冇 reconfigure 的 stream（測試／IDE 常見）
    try:
        console.setup()           # 唔可以 raise
    finally:
        sys.stdout = old


def test_safe_returns_printable_text(monkeypatch):
    _buf, stream = _stream(tty=True)
    monkeypatch.setattr(sys, "stdout", stream)
    out = console.safe("喺")
    assert isinstance(out, str)   # 唔會 raise，回一個一定印得出的字串


# ── settings.public() ───────────────────────────────────

def test_public_strips_comment_keys():
    conf = {
        "_comment": "說明",
        "_comment_server": "說明",
        "server": {"port": 8010, "_why": "註解"},
        "device": {"autoconnect": "auto"},
    }
    pub = cfg.public(conf)
    assert list(pub) == ["server", "device"]
    assert pub["server"] == {"port": 8010}
    assert pub["device"] == {"autoconnect": "auto"}


def test_public_does_not_mutate_input():
    conf = {"_x": 1, "server": {"port": 1}}
    cfg.public(conf)
    assert "_x" in conf


# ── run.py --print-config 不再爆，而且冇註解 ──────────────

def test_print_config_has_no_comment_keys(monkeypatch, capsys):
    import run

    rc = run.main(["--print-config"])
    assert rc == 0
    out = capsys.readouterr().out
    assert "_comment" not in out
    data = json.loads(out)
    assert "effective" in data and "used" in data
    assert all(not k.startswith("_") for k in data["effective"])
