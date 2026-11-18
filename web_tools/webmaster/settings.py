#!/usr/bin/env python3
"""WebMaster 設定檔：`web_tools/settings.json`（＋選用 `web_tools/settings.local.json`）。

位置
----
設定檔同**啟動器同層**（`web_tools/`），唔係收喺程式入面：

    web_tools/
    ├─ WebMaster.bat  web.ps1  web.sh  WebMaster.command   ← 啟動器
    ├─ settings.json          ← 你在呢度改（團隊預設，入 git）
    ├─ settings.local.json    ← 只係你部機（選用，唔入 git）
    └─ webmaster/             ← 程式本體（settings.py 喺呢度讀上面兩個檔）

理由：你要改嘅東西（port／預設連邊個裝置）應該喺「打開工具」那一層就見到，
唔需要入到程式目錄搵。

原則
----
1. **一個檔案講清楚「基礎數值」**：port／host／預設連邊個裝置／esptool 預設／
   瀏覽器行為。唔好再散落喺 shell、PowerShell、python 三處各有各預設。
2. **預設池**：本檔的 `DEFAULTS` 係「冇填就用呢個」的魚池。`settings.json` 可以
   **只寫你想改的 key**，其餘自動落返預設（深層合併，唔需要抄全文）。
3. **優先次序：命令列 > settings.local.json > settings.json > 內建預設。**
4. **壞值要大聲**：打錯 key 名或者型別錯 → 直接報錯並指出邊個 key，唔好靜靜用預設。
   （打錯 `"prot": 8010` 而靜靜開 8010 係最難查的一種。）
5. **未知 key 只警告**：容許你寫註解、或者預留未實作的欄位；但一定要見到警告。

點用
----
    python run.py --print-config        # 睇合併後的實際值 + 來源
    python run.py --write-config        # 寫一份帶註解的 settings.json（唔覆蓋已存在）
    python run.py --sim                 # 正常跑；CLI 永遠贏設定檔
"""
from __future__ import annotations

import copy
import json
import os
import sys
from pathlib import Path
from typing import Any

BASE = Path(__file__).resolve().parent          # …/web_tools/webmaster
TOOL_ROOT = BASE.parent                          # …/web_tools（設定檔所在）
SETTINGS_NAME = "settings.json"
LOCAL_NAME = "settings.local.json"

# ── 內建預設（settings.json 缺失時仍然跑得；刻意係「安全的最小值」）──────────
# 注：settings.json 係**團隊預設**，可以同呢度唔同（例如 autoconnect 設 "COM13"，
# 令雙擊就即刻有畫面）。優先次序：CLI > settings.local.json > settings.json > 呢度。
DEFAULTS: dict[str, Any] = {
    "server": {
        "host": "127.0.0.1",
        "port": 8010,
        "auto_port": True,       # port 被佔用就自動揀下一個（唔會卡住啟動）
        "auto_port_scan": 20,    # 最多試幾個
        "open_browser": True,
        "log_level": "info",
    },
    "device": {
        # "auto"=自動揀插住嘅真板（搵唔到就唔連，唔會偷偷開模擬）
        # "sim"=模擬裝置 / "COM13"、"/dev/cu.usbmodem…"=指定埠 / null=唔自動連
        "autoconnect": "auto",
        "prefer_port": None,     # auto 時優先揀呢個埠（例如 "COM20"）
        "baud": 115200,
        "sim_dir": None,
    },
    "flash": {
        "esptool_chip": "",      # 空 = 由 esptool 自動判斷
        "esptool_baud": 460800,
        "esptool_before": "usb_reset",
        "esptool_offset": "0x0",
        # 是否容許「頁面燒錄」（esptool-js／Web Serial）。只影響固件頁的第三張卡，
        # 預設開，但一定要用戶手動按「交出序列埠」才會生效。
        "browser_flash": True,
    },
    "ui": {
        "default_view": "device",
    },
    "env": {
        # off=只檢查唔裝；safe=自動補便宜又必需的（deps／esptool）；
        # all=連 ESP-IDF 都自動裝（1–2 GB，開機背景跑）。
        # 預設 all：開機就檢查，缺咩自動補（背景、失敗冷卻 6 小時）。
        "auto_install": "all",
        "idf_version": None,          # None=最新正式版；例 "v6.1.1"（v6.1.0 唔存在）
        "idf_targets": ["esp32s3", "esp32c3", "esp32c6", "esp32p4"],
        # None=自動揀一個**可寫**的位置（~/esp → %LOCALAPPDATA%\esp → ~/.espressif）
        # 公司機／受限環境若果唔准寫家目錄，設呢個去 D:\esp\esp-idf 之類。
        "idf_dir": None,
    },
}

# 型別檢查表：(路徑, 型別, 允許值)
_SCHEMA: list[tuple[str, type | tuple[type, ...], Any]] = [
    ("server.host", str, None),
    ("server.port", int, (1, 65535)),
    ("server.auto_port", bool, None),
    ("server.auto_port_scan", int, (1, 200)),
    ("server.open_browser", bool, None),
    ("server.log_level", str, ("critical", "error", "warning", "info", "debug", "trace")),
    ("device.autoconnect", (str, type(None)), None),
    ("device.prefer_port", (str, type(None)), None),
    ("device.baud", int, (300, 4_000_000)),
    ("device.sim_dir", (str, type(None)), None),
    ("flash.esptool_chip", str, None),
    ("flash.esptool_baud", int, (9600, 4_000_000)),
    ("flash.esptool_before", str, ("usb_reset", "default_reset", "no_reset")),
    ("flash.esptool_offset", (str, int), None),
    ("flash.browser_flash", bool, None),
    ("ui.default_view", str, ("device", "ide", "drive", "flash", "monitor")),
    ("env.auto_install", str, ("off", "safe", "all")),
    ("env.idf_version", (str, type(None)), None),
    ("env.idf_targets", list, None),
    ("env.idf_dir", (str, type(None)), None),
]

# 唔應該當成「打錯 key」的輔助欄位（同 make_config.json 一樣用 `_` 開頭）
_SOFT_PREFIX = "_"


class SettingsError(ValueError):
    """設定檔內容有問題（訊息要直接俾用戶睇）。"""


def warn(msg: str) -> None:
    print(f"[settings] 警告：{msg}", file=sys.stderr)


def public(conf: dict) -> dict:
    """去掉 `_comment` 這類說明欄位 → 只留真正的設定值。

    兩個用途：
    1. `--print-config` 同 `/api/config` 唔應該把註解當成「設定值」倒出來；
    2. 註解是自由中文，可能含 Big5 編不出的字（例如「喺」），而 Windows 主控台
       是 cp950 —— 少印就少一個爆點（雖然 `console.setup()` 已經保底）。
    """
    out: dict = {}
    for k, v in conf.items():
        if isinstance(k, str) and k.startswith("_"):
            continue
        out[k] = public(v) if isinstance(v, dict) else v
    return out


def settings_path(base: Path | None = None) -> Path:
    """`settings.json` 在**啟動器同層**（web_tools/），唔係喺 webmaster/。"""
    return (base or TOOL_ROOT) / SETTINGS_NAME


def local_path(base: Path | None = None) -> Path:
    return (base or TOOL_ROOT) / LOCAL_NAME


def _deep_merge(dst: dict, src: dict) -> dict:
    for k, v in src.items():
        if isinstance(v, dict) and isinstance(dst.get(k), dict):
            _deep_merge(dst[k], v)
        else:
            dst[k] = copy.deepcopy(v)
    return dst


def _flatten(d: dict, prefix: str = "") -> dict[str, Any]:
    out: dict[str, Any] = {}
    for k, v in d.items():
        key = f"{prefix}{k}"
        if isinstance(v, dict):
            out.update(_flatten(v, key + "."))
        else:
            out[key] = v
    return out


def _validate(merged: dict, origin: dict[str, str]) -> None:
    """逐個 key 檢查型別／範圍；錯就直接 raise（帶 key 名同來源檔）。"""
    flat = _flatten(merged)
    known = {s[0] for s in _SCHEMA}

    for key, typ, rng in _SCHEMA:
        if key not in flat:
            continue
        val = flat[key]
        where = origin.get(key, "?")

        # bool 係 int 的子類：`"port": true` 唔可以當成合法整數
        if typ is int and isinstance(val, bool):
            raise SettingsError(f"{key} 要整數，唔係 true/false（{where}）")
        if typ is bool and not isinstance(val, bool):
            raise SettingsError(f"{key} 要 true/false，收到 {val!r}（{where}）")

        if isinstance(typ, tuple):
            allowed_none = type(None) in typ
            if val is None:
                if not allowed_none:
                    raise SettingsError(f"{key} 唔可以係 null（{where}）")
            elif not any(isinstance(val, t) for t in typ if t is not type(None)):
                raise SettingsError(f"{key} 型別唔啱：{val!r}（{where}）")
        elif typ is list:
            if not isinstance(val, list):
                raise SettingsError(f"{key} 要清單（JSON array），收到 {val!r}（{where}）")
            if not all(isinstance(x, str) for x in val):
                raise SettingsError(f"{key} 每項都要係字串（{where}）")
        elif not isinstance(val, typ):
            raise SettingsError(f"{key} 要 {typ.__name__}，收到 {val!r}（{where}）")

        if val is None:
            continue
        if isinstance(rng, tuple) and rng and isinstance(rng[0], int) and not isinstance(rng[0], bool):
            lo, hi = rng
            if not lo <= val <= hi:
                raise SettingsError(f"{key}={val} 超出範圍 {lo}–{hi}（{where}）")
        elif isinstance(rng, tuple) and rng and isinstance(rng[0], str):
            if val not in rng:
                raise SettingsError(
                    f"{key}={val!r} 唔係允許值之一：{', '.join(rng)}（{where}）")

    unknown = [k for k in flat
               if k not in known and not k.split(".")[-1].startswith(_SOFT_PREFIX)]
    if unknown:
        warn(f"唔認識的 key（已忽略）：{', '.join(sorted(unknown))}"
             f"（{origin.get(unknown[0], '?')}）")


def _read_json(path: Path) -> dict:
    try:
        raw = path.read_text(encoding="utf-8")
    except OSError as e:
        raise SettingsError(f"讀唔到 {path.name}：{e}") from e
    try:
        data = json.loads(raw)
    except json.JSONDecodeError as e:
        raise SettingsError(f"{path.name} 唔係合法 JSON：第 {e.lineno} 行第 {e.colno} 欄：{e.msg}") from e
    if not isinstance(data, dict):
        raise SettingsError(f"{path.name} 最外層要係 object")
    return data


def load(base: Path | None = None, *, quiet: bool = False) -> tuple[dict, list[str]]:
    """讀設定 → (合併後的值, 用到的檔案清單)。

    `base` 係**放設定檔的資料夾**（預設 = web_tools/，即啟動器同層）。
    settings.json 可以只寫想改的 key —— 其餘自動落返 `DEFAULTS` 預設池。

    raise SettingsError（內容有錯）。
    """
    base = base or TOOL_ROOT
    merged = copy.deepcopy(DEFAULTS)
    origin: dict[str, str] = {k: "內建預設" for k in _flatten(DEFAULTS)}
    used: list[str] = []

    for path, label in ((settings_path(base), SETTINGS_NAME),
                        (local_path(base), LOCAL_NAME)):
        if not path.exists():
            continue
        data = _read_json(path)
        _deep_merge(merged, data)
        used.append(label)
        for k in _flatten(data):
            origin[k] = label

    # 環境變數（只支援幾個最常 CI／Docker 用的）
    env_map = {
        "ESPWM_HOST": ("server.host", str),
        "ESPWM_PORT": ("server.port", int),
        "ESPWM_SERIAL": ("device.autoconnect", str),
        "ESPWM_AUTOCONNECT": ("device.autoconnect", str),
        "ESPWM_LOG_LEVEL": ("server.log_level", str),
    }
    for env, (key, typ) in env_map.items():
        raw = os.environ.get(env)
        if raw is None or raw == "":
            continue
        try:
            val = typ(raw)
        except ValueError as e:
            raise SettingsError(f"環境變數 {env}={raw!r} 型別唔啱：{e}") from e
        node = merged
        parts = key.split(".")
        for p in parts[:-1]:
            node = node.setdefault(p, {})
        node[parts[-1]] = val
        origin[key] = f"env {env}"
        if "env" not in used:
            used.append("env")

    _validate(merged, origin)
    if not quiet:
        merged["_meta"] = {"files": used, "defaults": "內建預設"}
    return merged, used


def find_free_port(host: str, port: int, scan: int = 20) -> int | None:
    """由 port 開始搵第一個 bind 得到嘅 port；搵唔到回 None。"""
    import socket
    for candidate in range(port, port + max(1, scan) + 1):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            try:
                s.bind((host, candidate))
                return candidate
            except OSError:
                continue
    return None


def port_in_use(host: str, port: int) -> bool:
    import socket
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            s.bind((host, port))
            return False
        except OSError:
            return True


DEFAULT_FILE_TEXT = """{
  "_comment": "ESP WebMaster 設定檔（放喺 web_tools/，同啟動器同層）。優先次序：命令列 > settings.local.json > 本檔 > 程式內建預設池。",
  "_comment_partial": "可以只寫你想改的 key，其餘自動落返預設（深層合併），唔需要抄全文。",
  "_comment_server": "auto_port=true：port 被佔用就自動揀下一個（唔會卡住啟動）。",
  "server": {
    "host": "127.0.0.1",
    "port": 8010,
    "auto_port": true,
    "auto_port_scan": 20,
    "open_browser": true,
    "log_level": "info"
  },
  "_comment_device": "autoconnect：\\"auto\\"=自動連插住嘅真板（搵唔到就唔連，唔會開模擬）；\\"sim\\"=模擬裝置（免硬件）；\\"COM13\\"／\\"/dev/cu.usbmodem…\\"=指定序列埠；null=唔自動連，開 UI 自己揀。prefer_port 只在 auto 時用作優先。",
  "device": {
    "autoconnect": "auto",
    "prefer_port": null,
    "baud": 115200,
    "sim_dir": null
  },
  "_comment_flash": "ESPTOOL（固件頁第 3 卡）的預設值；空字串 = 由 esptool 自動判斷。browser_flash=頁面燒錄（esptool-js）開關。",
  "flash": {
    "esptool_chip": "",
    "esptool_baud": 460800,
    "esptool_before": "usb_reset",
    "esptool_offset": "0x0",
    "browser_flash": true
  },
  "ui": {
    "default_view": "device"
  },
  "_comment_env": "auto_install：off=只檢查；safe=開機自動補 deps／esptool；all=連 ESP-IDF 都自動裝（背景，10–30 分鐘）。idf_targets 用陣列，例如 esp32s3／esp32c3／esp32c6／esp32p4；idf_version 例如 v6.1.1（注意：冇 v6.1.0 這個 tag）。",
  "env": {
    "auto_install": "all",
    "idf_version": null,
    "idf_targets": ["esp32s3", "esp32c3", "esp32c6", "esp32p4"],
    "idf_dir": null
  }
}
"""


def write_default(base: Path | None = None, *, force: bool = False) -> Path:
    """寫一份 settings.json。已存在而冇 force → 唔覆蓋，直接回原路徑。"""
    path = settings_path(base)
    if path.exists() and not force:
        return path
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(DEFAULT_FILE_TEXT, encoding="utf-8")
    return path
