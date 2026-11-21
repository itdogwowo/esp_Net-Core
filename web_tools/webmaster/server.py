#!/usr/bin/env python3
"""ESP WebMaster —— 獨立 FastAPI 服務（唔依賴 mp_Net-Core）。

職責：開一條 CDC（或模擬裝置）、做 NC4 請求、將結果變成 REST/WS 給瀏覽器。
所有 device 操作經一把 lock 序列化（NC4 係一條線）。

檔案主路徑係 CDC FAT（0x2001–0x200B）：冇 USB driver 的 MCU 都通。
`/api/drive/*` 只係有 USB 的板子的可選 MSC 捷徑。
"""
from __future__ import annotations

import asyncio
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path
from typing import Any

from fastapi import Body, FastAPI, Header, HTTPException, Query, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse, JSONResponse, Response
from fastapi.staticfiles import StaticFiles

import container
import env
import events
import files
import flash
import formatting
import nc4
import ota
import scripts
import settings
from link import LinkError, SerialLink, SimLink, find_board_port, is_likely_board, list_ports
from sim import SimDevice

BASE_DIR = Path(__file__).resolve().parent
STATIC_DIR = BASE_DIR / "static"

# 目錄結構（2026-11-18 起）：
#   <repo>/web_tools/                 ← 啟動腳本（web.sh / web.ps1 / WebMaster.bat…）
#   <repo>/web_tools/webmaster/       ← 本檔所在（server＋static）
#   <repo>/                           ← repo 根（make.py / build.sh …）
# 「啟動」頁要同時列出呢兩個地方嘅腳本，所以帶兩條根（每次 request 現算，
# 測試會 monkeypatch REPO_ROOT）。
REPO_ROOT = Path(os.environ.get("ESPWM_REPO_ROOT") or BASE_DIR.parent.parent).resolve()
# 頁面燒錄請求的暫存映像（用完即刪）。env 可覆寫；測試會 monkeypatch 佢。
FLASH_DIR = Path(os.environ.get("ESPWM_FLASH_DIR") or (BASE_DIR / ".flashq"))


def script_roots() -> list[Path]:
    """啟動腳本要掃嘅根：`<repo>/web_tools` 同 repo 根（後者放建置腳本）。"""
    return [REPO_ROOT / "web_tools", REPO_ROOT]


COMMANDS = [
    {"cmd": "0x1101", "name": "STATUS_GET", "fields": ["keys"]},
    {"cmd": "0x1103", "name": "STATUS_SET", "fields": ["json", "persist"]},
    {"cmd": "0x100F", "name": "REBOOT", "fields": ["delay_ms"]},
    {"cmd": "0x2001", "name": "FILE_BEGIN", "fields": ["file_id", "total", "chunk", "sha256", "path"]},
    {"cmd": "0x2002", "name": "FILE_CHUNK", "fields": ["file_id", "offset", "data"]},
    {"cmd": "0x2003", "name": "FILE_END", "fields": ["file_id"]},
    {"cmd": "0x2005", "name": "FILE_QUERY", "fields": ["path"]},
    {"cmd": "0x2007", "name": "FILE_READ", "fields": ["path", "offset", "length"]},
    {"cmd": "0x2008", "name": "FILE_CONFIRM", "fields": ["path"]},
    {"cmd": "0x2009", "name": "FILE_DELETE", "fields": ["path"]},
    {"cmd": "0x200A", "name": "FILE_UNDO", "fields": ["path"]},
    {"cmd": "0x200B", "name": "FILE_SCAN", "fields": ["target"]},
    {"cmd": "0xF060", "name": "MATRIX", "fields": ["op", "..."], "test": True},
    {"cmd": "0x221B", "name": "OTA_STATE", "fields": []},
]


class Device:
    """一個已連線的裝置：自己的 link、lock、log ring、session。"""

    def __init__(self, key: str, link, port: str, sim: bool):
        self.key = key
        self.link = link
        self.port = port
        self.sim = sim
        self.lock = threading.RLock()
        self.events = events.EventLog(4000)
        self.session = f"{int(time.time() * 1000):x}-{os.getpid():x}-{key}"


class State:
    def __init__(self, sim_root: Path | None):
        self._admin = threading.RLock()
        self.devices: dict[str, Device] = {}
        self.active: str | None = None
        self._empty_events = events.EventLog(8)
        self._fallback_lock = threading.RLock()
        self.sim_root = sim_root or (BASE_DIR / ".simfs")
        self.loop: asyncio.AbstractEventLoop | None = None
        self.clients: set[WebSocket] = set()
        # 開機自動連線失敗的「人話」原因（例如埠被另一個程式佔用）；
        # 有值就代表「設定叫你連，但連唔到」，UI 要顯示出嚟，唔可以靜靜當冇事。
        self.autoconnect_error: str | None = None
        # 序列埠交接（頁面燒錄 esptool-js 用）：
        #   None = server 自己揸住個埠
        #   {"port": "COM20", "at": epoch, "device": key} = 交咗畀瀏覽器
        # 呢段時間 server **唔會**自己重開埠，否則會同瀏覽器搶。
        self.handover: dict[str, Any] | None = None
        # 頁面燒錄請求（AI 下指令 → 人按一下 → 瀏覽器燒 → 結果回給 AI）。
        # 為什麼要人呢一下：Web Serial 的 requestPort() 必須有 user gesture，
        # 呢個係瀏覽器安全模型，唔可以（亦唔應該）繞。所以 AI 只能「排隊請求」。
        self.flash_request: dict[str, Any] | None = None
        self.flash_history: list[dict[str, Any]] = []
        # 最近一次 /api/build 的結果：GET /api/build/info 會帶返去，UI reload 都知。
        self.last_build: dict[str, Any] | None = None
        # 同一時間只准一個 build：兩個 idf.py 一齊寫同一個 build dir 一定死。
        self.build_lock = threading.Lock()

    # ── 作用中裝置的 facade（現有 endpoint 不用逐個改）──
    @property
    def dev(self) -> Device | None:
        return self.devices.get(self.active) if self.active else None

    @property
    def link(self):
        return self.dev.link if self.dev is not None else None

    @property
    def port(self) -> str | None:
        return self.dev.port if self.dev is not None else None

    @property
    def is_sim(self) -> bool:
        return self.dev.sim if self.dev is not None else False

    @property
    def lock(self):
        return self.dev.lock if self.dev is not None else self._fallback_lock

    @property
    def events(self):
        return self.dev.events if self.dev is not None else self._empty_events

    @property
    def session(self) -> str:
        return self.dev.session if self.dev is not None else ""

    def device_list(self) -> list[dict[str, Any]]:
        return [{"device": d.key, "port": d.port, "sim": d.sim,
                 "connected": bool(d.link and d.link.connected),
                 "active": d.key == self.active}
                for d in self.devices.values()]

    def bind(self, d: Device) -> None:
        d.link.on_line = lambda text: self.log_line(d, text)
        d.link.on_frame = lambda cmd, payload: self.log_frame(d, cmd, payload)
        d.link.on_close = lambda reason: self.log_close(d, reason)

    def log_line(self, d: Device, text: str) -> None:
        ev = d.events.add("line", text, device=d.key)
        self.broadcast(ev)

    def log_frame(self, d: Device, cmd: int, payload: bytes) -> None:
        # 大型資料幀只記長度，避免把 16 KB payload 塞進 log ring。
        hexdata = payload.hex() if len(payload) <= 256 else ""
        ev = d.events.add("frame", cmd=f"0x{cmd:04X}", hex=hexdata,
                          len=len(payload), device=d.key)
        self.broadcast(ev)

    def log_close(self, d: Device, reason: str) -> None:
        self.broadcast({"type": "disconnected", "device": d.key, "reason": reason})

    def activity(self, actor: str, action: str, **detail: Any) -> None:
        d = self.dev
        if d is None:
            return
        ev = d.events.add("activity", actor=actor, action=action,
                          device=d.key, **detail)
        self.broadcast(ev)

    def broadcast(self, ev: dict[str, Any]) -> None:
        if self.loop is None or not self.clients:
            return
        if "device" not in ev and self.active:
            ev = {**ev, "device": self.active}

        async def go() -> None:
            for ws in list(self.clients):
                try:
                    await ws.send_json(ev)
                except Exception:
                    self.clients.discard(ws)

        try:
            asyncio.run_coroutine_threadsafe(go(), self.loop)
        except RuntimeError:
            pass

    def require(self):
        if self.link is None or not self.link.connected:
            if self.handover is not None:
                raise HTTPException(
                    status_code=409,
                    detail="序列埠已交畀瀏覽器（頁面燒錄中）。燒完之後按「交還序列埠」，"
                           "或者喺固件頁按「重新連線」。")
            raise HTTPException(status_code=409, detail="未連線：先去左邊揀 port")
        return self.link



def _json_or_none(payload: bytes):
    text = nc4.decode_str_u16(payload)
    if text is None:
        return None
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        return None


def _deep_copy_for_validate(d: dict) -> dict:
    import copy
    return copy.deepcopy(d)


def _merge_for_validate(d: dict) -> dict:
    """把「只寫了 env 的 local 設定」合併到預設上，才做 schema 驗證。"""
    import copy
    merged = copy.deepcopy(settings.DEFAULTS)
    _deep = settings._deep_merge
    return _deep(merged, d)


def _friendly_connect_error(port: str, e: Exception) -> str:
    """把「自動連線失敗」翻譯成人話（呢個位最易令人以為板壞）。"""
    text = str(e)
    low = text.lower()
    busy_hints = ("access is denied", "permissionerror", "拒絕存取", "存取被拒",
                  "busy", "cannot open", "could not open", "unable to open")
    if any(h in low for h in busy_hints) or any(h in text for h in busy_hints):
        return (f"連唔到 {port}：個埠好似被另一個程式佔用"
                f"（另一個 WebMaster／Arduino 監控／PuTTY？）。關掉佢再按「連線」。")
    if "timeout" in low:
        return f"連唔到 {port}：開埠 timeout。確認板已插好、驅動正常。"
    return f"連唔到 {port}：{text}"


def _connect(state: State, body: dict) -> dict:
    sim = bool(body.get("sim"))
    port = "sim" if sim else str(body.get("port") or "").strip()
    if not port:
        raise HTTPException(status_code=400, detail="缺 port")
    key = "sim" if sim else port

    with state._admin:
        d = state.devices.get(key)
        if d is not None and d.link is not None and d.link.connected:
            if bool(body.get("activate")):
                state.active = key
            return {"ok": True, "port": d.port, "sim": d.sim,
                    "device": d.key, "active": state.active,
                    "devices": state.device_list()}

        if d is not None and d.link is not None:
            try:
                d.link.close()
            except Exception:
                pass
            d.link = None

        try:
            if sim:
                link = SimLink(SimDevice(state.sim_root))
            else:
                link = SerialLink(port)
        except LinkError as e:
            raise HTTPException(status_code=400, detail=str(e))

        if d is None:
            d = Device(key, link, port, sim)
            state.devices[key] = d
        else:
            d.link = link
            d.port = port
            d.sim = sim
        state.bind(d)
        cur = state.dev
        cur_ok = cur is not None and cur.link is not None and cur.link.connected
        if bool(body.get("activate")) or state.active is None or not cur_ok:
            state.active = key
        state.broadcast({"type": "devices", "active": state.active,
                         "devices": state.device_list()})
    return {"ok": True, "port": d.port, "sim": d.sim,
            "device": d.key, "active": state.active,
            "devices": state.device_list()}


# ── 建置（repo 根 make.py）────────────────────────────────────────────
# 完整流程 =「改 /pixel → build → OTA → 讀 frame」。build 一定要喺 repo 根跑
# make.py（板子真相喺 ports/，project 選擇喺 ports/project/），所以由 server 用
# `sys.executable` 起子進程。**唔用 shell=True**：跨平台之外，board／project
# 都冇機會變成 shell 語法。
BUILD_TIMEOUT = 600.0          # 秒；任務要求最長 600s
BUILD_MAX_LINES = 4000         # 回傳行數上限（IDF build log 可以幾萬行）
BUILD_ARG_RE = re.compile(r"^[A-Za-z0-9._:/-]{1,160}$")
_ARTIFACT_TOKEN_RE = re.compile(r"([A-Za-z0-9_./\\-]+\.bin)")


def _as_text(value: Any) -> str:
    """TimeoutExpired.stdout 有時係 bytes（視 Python 版本）；統一轉字串。"""
    if value is None:
        return ""
    if isinstance(value, bytes):
        return value.decode("utf-8", "replace")
    return str(value)


def _read_json_file(path: Path) -> dict:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    return data if isinstance(data, dict) else {}


def _build_idf_candidates() -> list[tuple[Path, str]]:
    """跑 make.py 可能要用的 IDF 路徑＋來源（順序＝優先）。

    次序：`IDF_PATH` → settings 的 `env.idf_dir` → `~/esp/esp-idf` →
    `<repo>/.idf/esp-idf` → WebMaster env.py 的候選（非慣例位置）。
    呢個 function 只列「想試邊度」，係唔係真 IDF 由 `_looks_like_idf()` 判斷。
    """
    out: list[tuple[Path, str]] = []
    envdir = os.environ.get("IDF_PATH")
    if envdir:
        out.append((Path(envdir).expanduser(), "IDF_PATH"))
    try:
        conf, _used = settings.load(quiet=True)
        cfg_dir = (conf.get("env") or {}).get("idf_dir")
        if cfg_dir:
            out.append((Path(str(cfg_dir)).expanduser(), "settings.env.idf_dir"))
    except settings.SettingsError:
        pass
    except Exception:                       # 設定檔壞咗都唔應該令 build API 死
        pass
    out.append((Path.home() / "esp" / "esp-idf", "~/esp/esp-idf"))
    out.append((REPO_ROOT / ".idf" / "esp-idf", "<repo>/.idf/esp-idf"))
    try:
        for cand in env.idf_dir_candidates():
            out.append((Path(cand), "env.py 候選"))
    except Exception:
        pass
    seen: set[str] = set()
    uniq: list[tuple[Path, str]] = []
    for path, source in out:
        key = str(path)
        if key in seen:
            continue
        seen.add(key)
        uniq.append((path, source))
    return uniq


def _looks_like_idf(path: Path) -> bool:
    try:
        return path.is_dir() and (
            (path / "tools" / "idf.py").is_file()
            or (path / "export.sh").is_file()
            or (path / "export.ps1").is_file())
    except OSError:
        return False


def _resolve_build_idf() -> tuple[Path | None, str]:
    """第一個「似 IDF」的候選；冇就 (None, "")（make.py 自己會再報錯）。"""
    for path, source in _build_idf_candidates():
        if _looks_like_idf(path):
            return path, source
    return None, ""


def _scan_build_targets(repo: Path | None = None) -> dict:
    """掃 `ports/` 得出可建 boards／projects（唔跑 make.py，快又唔會卡）。

    板子真相：`ports/hardware/<chip>/<board>/board.json`
    project：`ports/project/<chip>/<project>/project.json`（`hardware` 指回板子）
    """
    repo = Path(repo if repo is not None else REPO_ROOT)
    boards: list[dict[str, Any]] = []
    hw_root = repo / "ports" / "hardware"
    try:
        chip_dirs = sorted(p for p in hw_root.iterdir() if p.is_dir())
    except OSError:
        chip_dirs = []
    for chip_dir in chip_dirs:
        try:
            board_dirs = sorted(p for p in chip_dir.iterdir() if p.is_dir())
        except OSError:
            continue
        for bdir in board_dirs:
            meta = _read_json_file(bdir / "board.json")
            if not meta:
                continue
            boards.append({
                "name": str(meta.get("name") or bdir.name),
                "chip": str(meta.get("chip") or chip_dir.name).lower(),
                "dir": f"{chip_dir.name}/{bdir.name}",
                "projects": [],
            })
    by_dir = {b["dir"]: b for b in boards}
    by_name = {b["name"]: b for b in boards if b["name"]}
    projects: list[str] = []
    proj_root = repo / "ports" / "project"
    try:
        p_chip_dirs = sorted(p for p in proj_root.iterdir() if p.is_dir())
    except OSError:
        p_chip_dirs = []
    for chip_dir in p_chip_dirs:
        try:
            pdirs = sorted(p for p in chip_dir.iterdir() if p.is_dir())
        except OSError:
            continue
        for pdir in pdirs:
            meta = _read_json_file(pdir / "project.json")
            if not meta:
                continue            # 未寫完／壞的 project 當唔存在（make.py 一樣）
            name = pdir.name
            if name not in projects:
                projects.append(name)
            hw = str(meta.get("hardware") or "")
            board = by_dir.get(hw) or by_name.get(hw) or by_name.get(name)
            if board is None:
                continue
            # default 慣例：project.json default:true，或者 project 同硬體板同名
            default = bool(meta.get("default")) or board["name"] == name
            board["projects"].append({"name": name, "default": default,
                                      "seed": meta.get("seed") or []})
    for board in boards:
        board["projects"].sort(key=lambda p: (not p["default"], p["name"]))
    return {"boards": boards, "projects": sorted(projects)}


def _clean_build_arg(value: Any, field: str) -> str:
    """board／project 參數驗證（唔用 shell 都照驗：唔想奇怪輸入走入 make.py）。"""
    text = str(value or "").strip()
    if not text:
        return ""
    if text.startswith("-") or not BUILD_ARG_RE.match(text):
        raise HTTPException(
            status_code=400,
            detail=f"{field} 格式唔啱：{text!r}（只准英數、._:-/，唔可以 - 開頭）")
    return text


def _artifact_meta(path: Path) -> dict:
    repo = Path(REPO_ROOT)
    try:
        rel = path.resolve().relative_to(repo.resolve()).as_posix()
    except (OSError, ValueError):
        rel = str(path)
    size = 0
    try:
        size = path.stat().st_size
    except OSError:
        pass
    return {"path": rel, "size": size, "abs": path}


def _find_artifact(lines: list[str], board: str, started: float) -> dict | None:
    """由 build output／檔案時間找出最新 image（make.py 成功時印「產物 …」）。

    次序＝可靠度：make.py 印的產物路徑 → 任何 artifacts/*.bin → 固定路徑 →
    artifacts/ 下新過 build 開始時間的最新 .bin（archive 關掉時的兜底）。
    """
    repo = Path(REPO_ROOT)

    def _is_app_bin(path: Path) -> bool:
        """跳過 build 內部的 bootloader／分割表／ota_data，只認 app image。"""
        low = path.name.lower()
        return path.is_file() and low.endswith(".bin") and not any(
            bad in low for bad in ("partition-table", "bootloader", "ota_data", "flash_args"))

    # 最可靠：make.py 成功時自己印的「產物 …」行（只認這一行，避免 build log 內
    # 的 `/artifacts/build/.../partition-table.bin` 被當成 artifact）。
    for line in lines:
        if "產物" not in line:
            continue
        match = _ARTIFACT_TOKEN_RE.search(line)
        if not match:
            continue
        token = Path(match.group(1))
        cand = token if token.is_absolute() else (repo / token)
        if _is_app_bin(cand):
            return _artifact_meta(cand)
    for line in lines:
        if "artifact" not in line.lower():
            continue
        match = _ARTIFACT_TOKEN_RE.search(line)
        if not match:
            continue
        token = Path(match.group(1))
        cand = token if token.is_absolute() else (repo / token)
        if _is_app_bin(cand):
            return _artifact_meta(cand)
    for line in lines:
        match = re.search(r"(?:^|[\s(（])(artifacts[/\\][^\s'\"，,]+\.bin)", line)
        if match:
            cand = repo / match.group(1).replace("\\", "/")
            if cand.is_file():
                return _artifact_meta(cand)
    for rel in (f"artifacts/build/{board}/esp_net_core.bin",
                f"artifacts/{board}/esp_net_core.bin",
                f"artifacts/{board}.bin"):
        cand = repo / rel
        if cand.is_file():
            return _artifact_meta(cand)
    out_dir = repo / "artifacts"
    try:
        bins = [p for p in out_dir.glob("*.bin") if p.is_file()]
    except OSError:
        bins = []
    fresh: list[Path] = []
    for path in bins:
        try:
            if path.stat().st_mtime >= started - 2:
                fresh.append(path)
        except OSError:
            continue
    if fresh:
        return _artifact_meta(max(fresh, key=lambda p: p.stat().st_mtime))
    return None


def _artifact_chip(artifact: str | None) -> str:
    """由 archive 的 sidecar（<name>.bin → <name>.json）讀 chip。"""
    if not artifact:
        return ""
    path = Path(REPO_ROOT) / str(artifact)
    meta = _read_json_file(path.with_suffix(".json"))
    chip = str(meta.get("chip") or meta.get("target") or "").strip().lower()
    return chip


def _chip_hint(repo: Path, board: str, project: str = "") -> str:
    """可由 board／project 參數猜到晶片（artifact sidecar 缺失時用）。"""
    query = (board or "").strip().lower()
    proj = (project or "").strip().lower()
    targets = _scan_build_targets(repo)
    for item in targets["boards"]:
        if query in (item["name"].lower(), item["dir"].lower()):
            return item["chip"]
    if proj:
        for item in targets["boards"]:
            for pj in item["projects"]:
                name = pj["name"].lower()
                if name == proj or name.endswith("-" + proj):
                    return item["chip"]
    return ""


def _safe_artifact_path(path: str) -> Path:
    """只准讀 `<repo>/artifacts` 入面的 .bin（防止 path traversal／誤讀其他檔）。"""
    rel = str(path or "").strip()
    if not rel:
        raise HTTPException(status_code=400, detail="缺 path")
    repo = Path(REPO_ROOT)
    artifacts = (repo / "artifacts").resolve()
    raw = Path(rel)
    cand = raw if raw.is_absolute() else (repo / rel)
    try:
        resolved = cand.resolve()
    except OSError:
        raise HTTPException(status_code=400, detail=f"路徑錯：{rel}")
    try:
        resolved.relative_to(artifacts)
    except ValueError:
        raise HTTPException(status_code=400,
                            detail="只准讀 repo/artifacts 入面嘅檔案")
    if resolved.suffix.lower() != ".bin":
        raise HTTPException(status_code=400, detail="只准 .bin")
    if not resolved.is_file():
        raise HTTPException(status_code=404,
                            detail=f"冇呢個 artifact：{rel}（先 build）")
    return resolved


def create_app(sim_root: str | Path | None = None,
               autoconnect: str | None = None) -> FastAPI:
    app = FastAPI(title="ESP WebMaster", version="1.0.0")
    state = State(Path(sim_root) if sim_root else None)

    # ── 連線 ──
    @app.get("/api/ports")
    def api_ports():
        """列出序列埠；`board:true` = 疑似 ESP 板（UI 排前＋預設揀佢）。

        判斷邏輯只有一份：`link.is_likely_board()`（VID 白名單＋排除 AMT／藍牙）。
        """
        ports = list_ports()
        for p in ports:
            p["board"] = is_likely_board(p)
            p["suggested"] = False
        pick = find_board_port()
        for p in ports:
            if pick and p["device"] == pick:
                p["suggested"] = True
                break
        return {"ok": True, "data": ports, "suggested": pick}

    @app.get("/api/info")
    def api_info():
        return {
            "ok": True,
            "connected": bool(state.link and state.link.connected),
            "port": state.port,
            "sim": state.is_sim,
            "device": state.active,
            "devices": state.device_list(),
            # 設定叫你連但連唔到（例：埠被另一個程式佔用）→ UI 要出聲
            "autoconnectError": state.autoconnect_error,
            "version": "1.0.0",
        }

    @app.get("/api/devices")
    def api_devices():
        return {"ok": True, "active": state.active, "devices": state.device_list()}

    @app.post("/api/select")
    def api_select(body: dict = Body(...)):
        key = str(body.get("device") or "")
        with state._admin:
            if key not in state.devices:
                raise HTTPException(status_code=404, detail="冇這台裝置")
            state.active = key
        state.broadcast({"type": "devices", "active": state.active,
                         "devices": state.device_list()})
        return {"ok": True, "active": state.active, "devices": state.device_list()}

    @app.post("/api/connect")
    def api_connect(body: dict = Body(...)):
        return _connect(state, body)

    @app.post("/api/disconnect")
    def api_disconnect(device: str = Query("")):
        with state._admin:
            key = device or state.active
            d = state.devices.pop(key, None) if key else None
            if d is not None and d.link is not None:
                try:
                    d.link.close()
                except Exception:
                    pass
            if key is not None and state.active == key:
                state.active = next(iter(state.devices), None)
        state.broadcast({"type": "devices", "active": state.active,
                         "devices": state.device_list()})
        return {"ok": True, "active": state.active, "devices": state.device_list()}

    # ── 序列埠交接（頁面燒錄 esptool-js）──────────────────────────────
    # 設計：序列埠同一時間只能有一個主人。要做瀏覽器燒錄，server 必須先**主動放開**，
    # 並記住「而家交咗出去」——呢段時間 server 唔會偷偷重開（否則就係搶埠）。
    # 交還（reclaim）之後 server 重新連線，一切照舊。
    @app.get("/api/port/state")
    def api_port_state():
        hs = state.handover
        port = (hs or {}).get("port") or state.port
        age = (time.time() - hs["at"]) if hs else None
        try:
            conf, _ = settings.load(quiet=True)
            browser_flash = bool(conf.get("flash", {}).get("browser_flash", True))
        except settings.SettingsError:
            browser_flash = False
        return {
            "ok": True,
            "handover": hs is not None,
            "port": port,
            "heldByBrowser": hs is not None,
            "ageSec": round(age, 1) if age is not None else None,
            # 超過 10 分鐘冇交還：可能係頁面關掉了 → UI 提示可以手動收回
            "stale": bool(age is not None and age > 600),
            "browserFlash": browser_flash,
        }

    @app.post("/api/port/release")
    def api_port_release(x_actor: str = Header("")):
        """server 放開序列埠，準備交畀瀏覽器的 Web Serial。"""
        with state._admin:
            d = state.dev
            if d is None or d.link is None:
                raise HTTPException(status_code=409, detail="而家冇連線，唔需要交還")
            port = d.port
            try:
                d.link.close()
            except Exception:
                pass
            d.link = None
            state.handover = {"port": port, "at": time.time(),
                              "device": d.key, "sim": bool(d.sim)}
        state.activity(x_actor or "ui", "port/release", port=port)
        state.broadcast({"type": "handover", "handover": True, "port": port})
        return {"ok": True, "handover": True, "port": port}

    @app.post("/api/port/reclaim")
    def api_port_reclaim(port: str = Query(""), x_actor: str = Header("")):
        """瀏覽器燒完 → 交還，server 重新連線。"""
        with state._admin:
            hs = state.handover or {}
            target = port or hs.get("port") or state.port
            # 交出去的可能係模擬裝置（測試／開發），交還時要照樣連模擬
            use_sim = (target == "sim") or (not port and bool(hs.get("sim")))
            state.handover = None
            if not target:
                raise HTTPException(status_code=400, detail="唔知要連返邊個埠")
            try:
                res = _connect(state, {"sim": True, "activate": True} if use_sim
                               else {"port": target, "activate": True})
            except HTTPException as e:
                state.autoconnect_error = f"交還之後連唔返 {target}：{e.detail}"
                raise HTTPException(
                    status_code=409,
                    detail=f"交還之後連唔返 {target}：{e.detail}。"
                           f"板子可能仍然喺 ROM／bootloader 模式，按 RESET 再試。")
        state.activity(x_actor or "ui", "port/reclaim", port=target)
        state.broadcast({"type": "handover", "handover": False, "port": target})
        return {"ok": True, "handover": False, **res}

    # ── 狀態 ──
    @app.get("/api/status")
    def api_status(keys: str = ""):
        link = state.require()
        with state.lock:
            try:
                cmd, rsp = link.request(0x1101, nc4.str_u16(keys),
                                        expect={0x1102, 0x2010}, timeout=6.0)
            except LinkError as e:
                raise HTTPException(status_code=504, detail=str(e))
        if cmd == 0x2010:
            raise HTTPException(status_code=409, detail="裝置拒絕 STATUS_GET")
        data = _json_or_none(rsp)
        return {"ok": True, "data": data, "raw": rsp.hex()}

    # ── 檔案 ──
    @app.get("/api/manifest")
    def api_manifest():
        link = state.require()
        with state.lock:
            try:
                data = files.fetch_manifest(link)
            except files.FileError as e:
                code = 404 if e.kind == "not_found" else 400
                raise HTTPException(status_code=code, detail={"kind": e.kind, "msg": str(e)})
            except LinkError as e:
                raise HTTPException(status_code=504, detail=str(e))
        return {"ok": True, "data": data}

    @app.post("/api/scan")
    def api_scan(target: int = 0):
        link = state.require()
        with state.lock:
            try:
                files.scan(link, target)
            except LinkError as e:
                raise HTTPException(status_code=504, detail=str(e))
        # 0x200B 冇回覆；UI 自己 poll /api/manifest
        return {"ok": True}

    @app.get("/api/file")
    def api_file_get(path: str = Query(...)):
        link = state.require()
        name = path.rstrip("/").split("/")[-1] or "file.bin"
        try:
            with state.lock:
                data = files.download(link, path)
        except files.FileError as e:
            raise HTTPException(status_code=404 if e.kind == "not_found" else 400,
                                detail={"kind": e.kind, "msg": str(e)})
        except LinkError as e:
            raise HTTPException(status_code=504, detail=str(e))
        return Response(content=data, media_type="application/octet-stream",
                        headers={"Content-Disposition": f'attachment; filename="{name}"'})

    @app.get("/api/container")
    def api_container(path: str = Query(...)):
        """讀 .nct／.nce（或任何 NCM 容器）→ 檔頭／manifest／meta／CRC。"""
        link = state.require()
        try:
            with state.lock:
                data = files.download(link, path)
        except files.FileError as e:
            raise HTTPException(status_code=404 if e.kind == "not_found" else 400,
                                detail={"kind": e.kind, "msg": str(e)})
        except LinkError as e:
            raise HTTPException(status_code=504, detail=str(e))
        return {"ok": True, "data": container.parse_container(data, path)}

    @app.put("/api/file")
    def api_file_put(path: str = Query(...), body: bytes = Body(b""),
                     x_actor: str = Header("")):
        link = state.require()
        total = len(body)

        def progress(done: int, all_: int):
            state.broadcast({"type": "progress", "op": "upload", "path": path,
                             "done": done, "total": all_})

        try:
            with state.lock:
                q = files.upload(link, path, body, progress=progress)
                auto_confirmed = False
                if q.get("pending"):
                    # 用戶唔會手動按確認：寫入成功即刻刪 .bak（confirm）。
                    # 如果 confirm 失敗，保留 pending=true 讓 UI 顯示／重試。
                    try:
                        q = files.confirm(link, path)
                        auto_confirmed = True
                    except (files.FileError, LinkError):
                        pass
        except files.FileError as e:
            raise HTTPException(status_code=400, detail={
                "kind": e.kind, "msg": str(e), **e.detail})
        except LinkError as e:
            raise HTTPException(status_code=504, detail=str(e))
        state.broadcast({"type": "file_changed", "path": q["path"],
                         "actor": x_actor or "api", "size": q["size"],
                         "deleted": False})
        return {
            "ok": True, "path": q["path"], "size": q["size"],
            "sha256": q["sha256"].hex(), "pending": q["pending"], "free": q["free"],
            "auto_confirmed": auto_confirmed,
        }

    @app.post("/api/file/delete")
    def api_file_delete(body: dict = Body(...), x_actor: str = Header("")):
        return _simple_file_op(state, files.delete, body.get("path", ""), x_actor, True)

    @app.post("/api/file/confirm")
    def api_file_confirm(body: dict = Body(...), x_actor: str = Header("")):
        return _simple_file_op(state, files.confirm, body.get("path", ""), x_actor)

    @app.post("/api/file/undo")
    def api_file_undo(body: dict = Body(...), x_actor: str = Header("")):
        return _simple_file_op(state, files.undo, body.get("path", ""), x_actor)

    # ── 固件 ──
    @app.post("/api/firmware")
    def api_firmware(apply: int = 1, expect_chip: str = "", body: bytes = Body(...)):
        link = state.require()

        # 安全鎖：S3 同 P4 的 TinyUSB CDC 都係 303A:4003，Windows 分唔出。
        # OTA 前用 STATUS_GET sys 對 chip；舊 firmware 冇 chip 就放行（UI 會再 confirm）。
        if expect_chip:
            with state.lock:
                try:
                    cmd, rsp = link.request(0x1101, nc4.str_u16("sys"),
                                            expect={0x1102, 0x2010}, timeout=4.0)
                except LinkError as e:
                    raise HTTPException(status_code=504, detail=str(e))
            if cmd == 0x1102:
                j = _json_or_none(rsp) or {}
                got = str(((j.get("sys") or {}).get("chip") or "")).lower()
                norm = lambda s: s.replace("-", "").replace("_", "")
                if got and norm(got) != norm(expect_chip):
                    raise HTTPException(status_code=409, detail={
                        "kind": "chip_mismatch", "board": got, "image": expect_chip,
                        "msg": f"板子係 {got}，image 係 {expect_chip}；拒絕 OTA",
                    })

        def progress(done: int, all_: int):
            state.broadcast({"type": "progress", "op": "ota", "done": done, "total": all_})

        try:
            with state.lock:
                res = ota.ota_update(link, body, apply=bool(apply), progress=progress)
        except ota.OtaError as e:
            raise HTTPException(status_code=400, detail={"names": e.names, **e.detail})
        except LinkError as e:
            raise HTTPException(status_code=504, detail=str(e))
        return {"ok": True, "data": res}

    # ── 建置（repo 根 make.py；完整流程的第一站）────────────────────
    @app.get("/api/build/info")
    def api_build_info():
        """可建咩：repo／make.py／IDF／boards／projects。只讀，唔會跑 build。"""
        repo = Path(REPO_ROOT)
        make_py = repo / "make.py"
        targets = _scan_build_targets(repo)
        idf, idf_source = _resolve_build_idf()
        candidates = [{"path": str(path), "source": source,
                       "exists": _looks_like_idf(path)}
                      for path, source in _build_idf_candidates()]
        return {"ok": True, "data": {
            "repo": str(repo),
            "makePy": "make.py",
            "makePyExists": make_py.is_file(),
            "python": sys.executable,
            "timeout": BUILD_TIMEOUT,
            "idfPath": str(idf) if idf is not None else None,
            "idfSource": idf_source,
            "idfCandidates": candidates,
            "boards": targets["boards"],
            "projects": targets["projects"],
            "canBuild": bool(make_py.is_file() and idf is not None),
            "lastBuild": state.last_build,
        }}

    @app.post("/api/build")
    def api_build(body: dict | None = Body(None), x_actor: str = Header("")):
        """跑 `make.py build <board> [--project …] [--tests] [--fresh]`。

        回 `{ok, returncode, lines, artifact, size, elapsed, …}`。build 失敗都係
        HTTP 200（`lines` 就係證據）；只有參數／make.py 缺失才 4xx。
        """
        data = body or {}
        if not isinstance(data, dict):
            raise HTTPException(status_code=400, detail="body 要係 JSON object")
        board = _clean_build_arg(data.get("board_query") or data.get("board"),
                                 "board")
        project = _clean_build_arg(data.get("project"), "project")
        if not board:
            raise HTTPException(
                status_code=400,
                detail="缺 board（板名／project 名／<chip>/<board>）")
        tests = bool(data.get("tests"))
        fresh = bool(data.get("fresh"))
        repo = Path(REPO_ROOT)
        make_py = repo / "make.py"
        if not make_py.is_file():
            raise HTTPException(status_code=400,
                                detail=f"搵唔到 {make_py}（repo 根要有 make.py）")

        cmd = [sys.executable, str(make_py), "build", board]
        if project:
            cmd += ["--project", project]
        if tests:
            cmd.append("--tests")
        if fresh:
            cmd.append("--fresh")

        # IDF_PATH：由 IDF_PATH 環境變數／settings／慣例目錄揀第一個似 IDF 的。
        # make.py 內部的 export_prefix 會自己砌 PATH，所以呢度只需要 IDF_PATH。
        child_env = dict(os.environ)
        idf, _idf_source = _resolve_build_idf()
        if idf is not None:
            child_env["IDF_PATH"] = str(idf)
        child_env.setdefault("PYTHONIOENCODING", "utf-8")
        child_env["PYTHONUTF8"] = "1"        # 子進程 output 用 UTF-8（Windows cp950 會爛）

        if not state.build_lock.acquire(blocking=False):
            raise HTTPException(status_code=409,
                                detail="已經有 build 喺跑，等佢完先")
        started = time.time()
        timed_out = False
        try:
            try:
                proc = subprocess.run(
                    cmd, cwd=str(repo), env=child_env,
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                    text=True, encoding="utf-8", errors="replace",
                    timeout=BUILD_TIMEOUT,
                )
                returncode = int(proc.returncode)
                output = proc.stdout or ""
            except subprocess.TimeoutExpired as exc:
                timed_out = True
                returncode = -1
                output = (_as_text(getattr(exc, "stdout", ""))
                          + _as_text(getattr(exc, "stderr", "")))
                output += f"\n✘ 建置超過 {BUILD_TIMEOUT:.0f}s，已中止。\n"
            except OSError as exc:
                returncode = -2
                output = f"✘ 執行 make.py 失敗：{exc}\n"
        finally:
            state.build_lock.release()

        elapsed = round(time.time() - started, 2)
        lines = output.splitlines()
        truncated = False
        if len(lines) > BUILD_MAX_LINES:
            lines = lines[-BUILD_MAX_LINES:]
            truncated = True
            lines.insert(0, f"（只顯示最後 {BUILD_MAX_LINES} 行）")
        artifact = _find_artifact(lines, board, started) if returncode == 0 else None
        chip = _artifact_chip(artifact["path"] if artifact else None)
        if not chip:
            chip = _chip_hint(repo, board, project)
        result = {
            "ok": returncode == 0,
            "returncode": returncode,
            "lines": lines,
            "truncated": truncated,
            "artifact": artifact["path"] if artifact else None,
            "size": int(artifact["size"]) if artifact else 0,
            "elapsed": elapsed,
            "timeout": timed_out,
            "maxLines": BUILD_MAX_LINES,
            "board": board,
            "project": project or None,
            "chip": chip,
            "repo": str(repo),
            "idf": str(idf) if idf is not None else None,
            "cmd": cmd,
        }
        state.last_build = {
            "board": board, "project": project or None, "tests": tests,
            "fresh": fresh, "ok": result["ok"], "returncode": returncode,
            "artifact": result["artifact"], "size": result["size"],
            "chip": chip, "elapsed": elapsed, "timeout": timed_out,
            "at": time.time(),
        }
        state.activity(x_actor or "ui", "build",
                       board=board, project=project or None, tests=tests,
                       fresh=fresh, ok=result["ok"], returncode=returncode,
                       artifact=result["artifact"], elapsed=elapsed)
        return result

    @app.get("/api/build/artifact")
    def api_build_artifact(path: str = Query(...)):
        """把 build 出的 .bin 回給瀏覽器（一鍵 OTA：fetch blob → POST /api/firmware）。"""
        file_path = _safe_artifact_path(path)
        return FileResponse(file_path, media_type="application/octet-stream",
                            filename=file_path.name,
                            headers={"Cache-Control": "no-store"})

    # ── 共享 log／activity（Web UI 與 AI 同一份）──
    def _events_of(device: str):
        if device:
            d = state.devices.get(device)
            if d is None:
                raise HTTPException(status_code=404, detail="冇這台裝置")
            return d.events
        return state.events

    @app.get("/api/log/tail")
    def api_log_tail(n: int = 200, since: int = 0, device: str = ""):
        return {"ok": True, "events": _events_of(device).tail(n=n, since=since)}

    @app.get("/api/activity")
    def api_activity(n: int = 100, since: int = 0, device: str = ""):
        return {"ok": True, "events": _events_of(device).tail(n=n, since=since, kind="activity")}

    @app.post("/api/log/note")
    def api_log_note(body: dict = Body(...)):
        actor = str(body.get("actor") or "ai")
        text = str(body.get("text") or "").strip()
        if not text:
            raise HTTPException(status_code=400, detail="缺 text")
        state.activity(actor, "note", text=text[:2000])
        return {"ok": True}

    # AI／工具用的能力清單（人類看 README，AI 看這個）。
    @app.get("/api/ai")
    def api_ai():
        return {
            "ok": True,
            "mode": "single-owner serial broker",
            "note": "序列埠由 server 獨佔；AI 不要再開 COM port，請用本 API。"
                    "請帶 X-Actor: ai，活動會出現在 UI 的 Activity。",
            "endpoints": [
                {"method": "GET", "path": "/api/info"},
                {"method": "GET", "path": "/api/devices"},
                {"method": "POST", "path": "/api/select", "body": {"device": "..."}},
                {"method": "GET", "path": "/api/log/tail?n=200&since=0&device=..."},
                {"method": "GET", "path": "/api/activity?n=100"},
                {"method": "POST", "path": "/api/log/note", "body": {"text": "..."}},
                {"method": "GET", "path": "/api/status?keys=sys,storage,log"},
                {"method": "GET", "path": "/api/manifest"},
                {"method": "POST", "path": "/api/scan?target=0"},
                {"method": "GET", "path": "/api/file?path=/config/system.json"},
                {"method": "PUT", "path": "/api/file?path=/config/x.json", "body": "raw bytes"},
                {"method": "POST", "path": "/api/file/confirm", "body": {"path": "..."}},
                {"method": "POST", "path": "/api/file/undo", "body": {"path": "..."}},
                {"method": "POST", "path": "/api/file/delete", "body": {"path": "..."}},
                {"method": "POST", "path": "/api/reboot"},
                {"method": "POST", "path": "/api/cmd", "body": {"cmd": "0x1101", "payload_hex": ""}},
                {"method": "POST", "path": "/api/firmware?apply=1&expect_chip=esp32s3", "body": "app bin"},
                {"method": "GET", "path": "/api/build/info"},
                {"method": "POST", "path": "/api/build", "body": {"board": "ESP32-S3-Matrix", "project": "ESP32-S3-Matrix-Show", "tests": False, "fresh": False}},
                {"method": "GET", "path": "/api/build/artifact?path=artifacts/ESP32-S3-Matrix.bin"},
                {"method": "POST", "path": "/api/cmd", "body": {"cmd": "0xF06B", "payload_hex": "", "expect": ["0xF06C"], "timeout": 5}},
                {"method": "GET", "path": "/api/flash/info?chip=esp32p4&before=usb_reset"},
                {"method": "POST", "path": "/api/flash/esptool?offset=0x0&baud=460800&chip=esp32p4&before=usb_reset", "body": "bin"},
                {"method": "GET", "path": "/api/container?path=/tasks/x.nct"},
                {"method": "GET", "path": "/api/scripts"},
                {"method": "GET", "path": "/api/scripts/one?name=web.sh"},
                {"method": "POST", "path": "/api/format", "body": {"path": "/config/x.json", "text": "..."}},
                {"method": "POST", "path": "/api/log/note", "body": {"text": "..."}},
            ],
        }

    @app.middleware("http")
    async def activity_middleware(request, call_next):
        started = time.monotonic()
        response = await call_next(request)
        try:
            path = request.url.path
            actor_hdr = request.headers.get("x-actor")
            mutating = request.method in {"POST", "PUT", "DELETE", "PATCH"}
            # AI 的讀取也要看得見（X-Actor: ai），但避免 log/activity 自己回音。
            quiet = {"/api/log/tail", "/api/activity", "/api/info", "/api/ports",
                     "/api/commands", "/api/ai", "/api/cmd", "/api/log/note",
                     "/api/format", "/api/scripts/one"}
            skip_mut = path in {"/api/cmd", "/api/log/note", "/api/flash/esptool",
                                "/api/build"}
            watch = (mutating and not skip_mut) or (actor_hdr == "ai" and path not in quiet)
            if path.startswith("/api/") and watch:
                actor = actor_hdr or ("ui" if request.headers.get("referer") else "api")
                detail = {k: v for k, v in request.query_params.items()}
                state.activity(actor, f"{request.method} {path}",
                               status=response.status_code,
                               ms=round((time.monotonic() - started) * 1000, 1),
                               **detail)
        except Exception:
            pass
        return response

    @app.get("/api/flash/info")
    def api_flash_info(chip: str = Query(""), before: str = Query("usb_reset")):
        if state.is_sim or not state.port:
            raise HTTPException(status_code=409, detail="模擬模式／未連線")
        d = state.dev
        if d is None:
            raise HTTPException(status_code=409, detail="未連線")
        port = d.port
        lines: list[str] = []
        try:
            with d.lock:
                if d.link is not None:
                    try:
                        d.link.close()
                    except Exception:
                        pass
                    d.link = None
            cmd = flash.build_info_cmd(port, chip=chip, before=before)
            rc = flash.run_flash(cmd, timeout=60,
                                 on_line=lambda line: lines.append(line))
            try:
                _connect(state, {"port": port})
            except Exception:
                pass
            return {"ok": rc == 0, "rc": rc, "lines": lines}
        except flash.FlashError as e:
            try:
                _connect(state, {"port": port})
            except Exception:
                pass
            raise HTTPException(status_code=504, detail=str(e))

    # ── ESPTOOL（ROM／USB-Serial-JTAG）──
    @app.post("/api/flash/esptool")
    def api_flash_esptool(offset: str = Query("0x0"), baud: int = Query(460800),
                          chip: str = Query(""), before: str = Query("usb_reset"),
                          erase: int = Query(0), image: bytes = Body(b""),
                          x_actor: str = Header("")):
        if state.is_sim or not state.port:
            raise HTTPException(status_code=409, detail="模擬模式／未連線：唔可以 esptool 燒錄")
        if not erase and not image:
            raise HTTPException(status_code=400, detail="冇 image 又冇 erase")
        d = state.dev
        if d is None:
            raise HTTPException(status_code=409, detail="未連線")
        port = d.port
        try:
            off = int(str(offset), 0)
        except ValueError:
            raise HTTPException(status_code=400, detail=f"offset 唔係整數：{offset}")
        tmp = None
        try:
            if image:
                fd, tmp = tempfile.mkstemp(suffix=".bin", prefix="espwm-")
                with os.fdopen(fd, "wb") as f:
                    f.write(image)
            cmd = flash.build_cmd(port, tmp, offset=off, baud=baud,
                                  chip=chip, before=before, erase=bool(erase))
            state.activity(x_actor or "ui", "FLASH esptool", port=port,
                           bytes=len(image), offset=hex(off), erase=bool(erase))
            # 放開序列埠（一個 port 只可以一個 owner），燒完再連返。
            with d.lock:
                if d.link is not None:
                    try:
                        d.link.close()
                    except Exception:
                        pass
                    d.link = None
            state.broadcast({"type": "flash", "phase": "start", "port": port,
                             "bytes": len(image)})
            rc = flash.run_flash(
                cmd,
                on_line=lambda line: state.broadcast({"type": "line", "text": line, "flash": True}),
                on_progress=lambda pct: state.broadcast({"type": "progress", "op": "esptool",
                                                         "done": pct, "total": 100}),
            )
            state.broadcast({"type": "flash", "phase": "done", "rc": rc})
            # 重新連線（硬重置後同一個 port 通常會返嚟；失敗就交由 UI 手動重連）
            try:
                _connect(state, {"port": port})
            except Exception as e:
                state.broadcast({"type": "disconnected", "reason": f"flash 後重連失敗：{e}"})
            if rc != 0:
                raise HTTPException(status_code=500, detail=f"esptool exit {rc}")
            return {"ok": True, "rc": rc}
        except flash.FlashError as e:
            try:
                _connect(state, {"port": port})
            except Exception:
                pass
            raise HTTPException(status_code=504, detail=str(e))
        finally:
            if tmp:
                try:
                    os.unlink(tmp)
                except Exception:
                    pass

    # ── 其他 ──
    @app.post("/api/reboot")
    def api_reboot():
        link = state.require()
        with state.lock:
            try:
                link.send(0x100F, struct.pack("<I", 0))
            except LinkError as e:
                raise HTTPException(status_code=504, detail=str(e))
        return {"ok": True}

    @app.get("/api/commands")
    def api_commands():
        return {"ok": True, "data": COMMANDS}

    # ── 設定（settings.json／settings.local.json，唯讀）──
    @app.get("/api/config")
    def api_config():
        """而家真正生效的基礎數值＋邊個檔提供。唔會回任何敏感資料。"""
        try:
            conf, used = settings.load(quiet=True)
        except settings.SettingsError as e:
            return {"ok": False, "detail": str(e)}
        return {
            "ok": True,
            "files": used,
            "path": str(settings.settings_path()),
            "localPath": str(settings.local_path()),
            "localExists": settings.local_path().exists(),
            # public() 去掉 `_comment` 這類說明欄位（唔係設定值）
            "effective": settings.public(conf),
        }

    # ── 啟動腳本（web_tools/ ＋ repo 根）──
    @app.get("/api/scripts")
    def api_scripts():
        """人用嘅啟動腳本清單：分類 → 平台 → 指令。唔會執行任何腳本。"""
        roots = script_roots()
        data = scripts.discover(roots)
        counts: dict[str, int] = {c: 0 for c, *_ in scripts.CATEGORIES}
        for s in data:
            counts[s["category"]] = counts.get(s["category"], 0) + 1
        return {
            "ok": True,
            "root": REPO_ROOT.name,
            "roots": [p.name for p in roots],
            "platforms": list(scripts.PLATFORMS),
            "categories": [{"key": k, "title": t, "desc": d, "icon": i}
                           for k, t, d, i in scripts.CATEGORIES],
            "counts": counts,
            "data": data,
        }

    @app.get("/api/scripts/one")
    def api_script_one(rel: str = Query(""), name: str = Query("")):
        """讀腳本全文。`rel` 由 repo 根起算（例如 `web_tools/web.sh`）；
        冇 `rel` 就用 `name` 在兩條根裡找第一個符合的。"""
        target = rel or name
        if not target:
            raise HTTPException(status_code=400, detail="要 rel 或 name")
        try:
            data = scripts.read(script_roots(), target)
        except ValueError as e:
            raise HTTPException(status_code=404, detail=str(e))
        return {"ok": True, "data": data}

    # ── 環境（doctor）＋安裝（IDF／deps／esptool）────────────────────
    @app.get("/api/env")
    def api_env():
        """環境自檢：缺咩、缺咗有咩後果、點補。只讀，唔會裝任何東西。"""
        rep = env.report()
        # 把「設定檔已選的晶片」也帶回去，UI 才可以預先勾好（例如 S31）
        try:
            conf, _used = settings.load(quiet=True)
            envcfg = conf.get("env") or {}
            rep["targets"]["configured"] = envcfg.get("idf_targets") or []
            rep["targets"]["configuredVersion"] = envcfg.get("idf_version")
            rep["targets"]["configuredDir"] = envcfg.get("idf_dir")
            mode = envcfg.get("auto_install") or "safe"
            rep["auto"]["mode"] = mode
            # 直接回「按呢個模式，開機時會做咩」——用戶見到「冇幫我裝」時，
            # 呢個欄位就係答案（blocked 會帶 hint）。
            try:
                rep["auto"]["plan"] = env.auto_plan(mode, rep)
            except Exception as e:                  # 計劃失敗唔應該令整頁爆
                rep["auto"]["plan"] = []
                rep["auto"]["planError"] = str(e)
        except settings.SettingsError as e:
            rep["targets"]["configured"] = []
            rep["settingsError"] = str(e)
        return {"ok": True, "data": rep}

    @app.get("/api/env/idf/versions")
    def api_idf_versions(force: int = 0, prerelease: int = 0):
        """ESP-IDF 可用版本（GitHub tags，快取 1 小時）。

        實測重要事實：**6.1.0 不存在**——ESP-IDF 用 6.1（發佈）+ 6.1.1（修補），
        冇 .0。所以前端要由呢個 API 取清單，唔好寫死。
        """
        info = env.fetch_idf_versions(force=bool(force))
        tags = info["tags"] if prerelease else env.stable_versions(info["tags"])
        best, note = env.normalize_version(None, tags)
        return {
            "ok": True,
            "data": {
                "versions": tags[:40],
                "latest": best,
                "error": info.get("error"),
                "cachedAt": info.get("at"),
                "note": note,
            },
        }

    @app.post("/api/env/config")
    def api_env_config(body: dict = Body(...), x_actor: str = Header("")):
        """把 env.* 寫入 **settings.local.json**（本機覆寫，唔會改團隊的 settings.json）。

        為什麼要這個 API：IDF 裝喺邊係「呢部機」的事（公司機／受限環境唔准寫家目錄），
        UI 一按就要生效，唔應該叫用戶手改 JSON。
        可用欄位：idf_dir / idf_version / auto_install / idf_targets。
        """
        allow = {"idf_dir", "idf_version", "auto_install", "idf_targets"}
        incoming = {k: v for k, v in (body or {}).items() if k in allow}
        if not incoming:
            raise HTTPException(status_code=400, detail=f"冇可寫欄位（可用：{sorted(allow)}）")
        local = settings.local_path()
        cur: dict = {}
        if local.exists():
            try:
                import json as _json
                cur = _json.loads(local.read_text(encoding="utf-8"))
                if not isinstance(cur, dict):
                    cur = {}
            except Exception:
                cur = {}
        env_sec = dict(cur.get("env") or {})
        env_sec.update(incoming)
        cur["env"] = env_sec
        # 先用 schema 驗一次，唔好寫入壞資料
        try:
            merged = _deep_copy_for_validate(cur)
            settings._validate(_merge_for_validate(merged), {})
        except settings.SettingsError as e:
            raise HTTPException(status_code=400, detail=str(e))
        try:
            import json as _json
            local.write_text(_json.dumps(cur, ensure_ascii=False, indent=2) + "\n",
                             encoding="utf-8")
        except OSError as e:
            raise HTTPException(status_code=500, detail=f"寫唔到 {local.name}：{e}")
        state.activity(x_actor or "ui", "env/config", **{k: v for k, v in incoming.items()})
        try:
            conf, used = settings.load(quiet=True)
        except settings.SettingsError as e:
            raise HTTPException(status_code=400, detail=str(e))
        return {"ok": True, "file": local.name, "files": used,
                "env": conf.get("env") or {}}

    @app.post("/api/env/install")
    def api_env_install(body: dict = Body(...), x_actor: str = Header("")):
        """跑官方安裝命令（deps／esptool／idf）。**一定要 confirm:true**。

        輸出唔會阻塞：用 GET /api/env/install 逐次讀新行（since=已讀行數）。
        """
        what = str(body.get("what") or "")
        # target 可以係 preset 名、逗號字串、或陣列（例如 ["esp32s3","esp32p4"]）
        raw_target = body.get("target", "all")
        if isinstance(raw_target, list):
            raw_target = ",".join(str(x) for x in raw_target)
        target = str(raw_target or "all")
        confirm = bool(body.get("confirm"))
        idf_dir = body.get("dir") or None
        version = body.get("version") or None
        try:
            snap = env.INSTALLER.start(
                what, target=target,
                idf_dir=Path(idf_dir) if idf_dir else None,
                confirm=confirm, version=version)
        except ValueError as e:
            raise HTTPException(status_code=400, detail=str(e))
        state.activity(x_actor or "ui", f"env/install {what}",
                       target=target, ok=True)
        return {"ok": True, "data": snap}

    @app.get("/api/env/install")
    def api_env_install_log(since: int = 0):
        return {"ok": True, "data": env.INSTALLER.snapshot(since)}

    # ── 頁面燒錄：AI 下指令 → 人按一下 → 瀏覽器燒 → 結果回 AI ──────
    # AI 本身已經可以經 ① NC4 OTA／② server esptool 燒錄；呢組 API 係為咗
    # 「③ 頁面燒錄」也可以由 AI 發起（例如 server 側冇 esptool、或者想用
    # 瀏覽器的埠）。唯一保留：真正嘅 `requestPort()` 必須由人手勢觸發，
    # 所以 AI 只能排隊，人等 UI 彈出時按一下。
    MAX_IMAGE = 8 * 1024 * 1024

    def _flash_dir() -> Path:
        d = FLASH_DIR
        d.mkdir(parents=True, exist_ok=True)
        return d

    def _req_public(req: dict) -> dict:
        return {k: v for k, v in req.items() if k not in ("path",)}

    @app.get("/api/flash/paths")
    def api_flash_paths():
        """三條燒錄路徑：邊條 AI 用得到、點解。"""
        rep = env.report()
        ready = rep.get("ready") or {}
        return {
            "ok": True,
            "data": {
                "paths": [
                    {"id": "nc4_ota", "label": "NC4 OTA",
                     "endpoint": "POST /api/firmware",
                     "aiCanInvoke": bool(ready.get("flash_nc4_ota")),
                     "needsPortHandover": False,
                     "requires": "firmware 有 NC4（S3／C3）",
                     "best": "日常更新，唔使放開序列埠"},
                    {"id": "server_esptool", "label": "server 側 esptool",
                     "endpoint": "POST /api/flash/esptool",
                     "aiCanInvoke": bool(ready.get("flash_esptool")),
                     "needsPortHandover": False,
                     "requires": "pip esptool＋ROM 模式",
                     "best": "P4、救磚、首燒 bootloader"},
                    {"id": "browser", "label": "頁面燒錄（esptool-js）",
                     "endpoint": "POST /api/flash/request → 人按一下 → UI 燒",
                     "aiCanInvoke": True,
                     "needsPortHandover": True,
                     "requires": "Chrome／Edge 89+、有人喺 UI 按確認",
                     "best": "server 側冇 esptool 時；AI 發起、人確認",
                     "note": "Web Serial 的 requestPort() 必須 user gesture，"
                             "所以 AI 只能排隊請求，最後一下一定要人按。"},
                ],
                "humanGestureRequired": ["browser"],
            },
        }

    @app.post("/api/flash/request")
    def api_flash_request(body: bytes = Body(b""),
                          offset: str = Query("0x0"),
                          flash_mode: str = Query("dio", alias="mode"),
                          flash_freq: str = Query("40m", alias="freq"),
                          flash_size: str = Query("4MB", alias="size"),
                          erase_all: int = Query(0),
                          note: str = Query(""),
                          x_actor: str = Header("")):
        """AI 發起頁面燒錄請求（上傳 .bin bytes）。等人喺 UI 按「執行」。"""
        if not body:
            raise HTTPException(status_code=400, detail="空檔案")
        if len(body) > MAX_IMAGE:
            raise HTTPException(status_code=413,
                                detail=f"映像太大（上限 {MAX_IMAGE // (1024*1024)} MiB）")
        try:
            off = int(offset, 0)
        except ValueError:
            raise HTTPException(status_code=400, detail=f"offset 唔合法：{offset!r}")
        if off < 0:
            raise HTTPException(status_code=400, detail="offset 唔可以係負數")
        with state._admin:
            if state.flash_request and state.flash_request.get("status") == "pending":
                raise HTTPException(
                    status_code=409,
                    detail="已經有一個待處理的燒錄請求；等佢完成或者取消再發。")
            rid = f"{int(time.time()*1000):x}"
            p = _flash_dir() / f"{rid}.bin"
            p.write_bytes(body)
            req = {
                "id": rid, "path": str(p), "size": len(body),
                "offset": off, "mode": flash_mode, "freq": flash_freq,
                "sizeOpt": flash_size, "eraseAll": bool(erase_all),
                "note": note[:300], "by": x_actor or "api",
                "status": "pending", "created": time.time(),
                "claimedAt": None, "finishedAt": None, "error": None,
            }
            state.flash_request = req
        state.activity(x_actor or "api", "flash/request",
                       id=rid, bytes=len(body), offset=hex(off), ok=True)
        state.broadcast({"type": "flash_request", **_req_public(req)})
        return {"ok": True, "data": _req_public(req)}

    @app.get("/api/flash/request")
    def api_flash_request_get():
        req = state.flash_request
        return {"ok": True, "data": _req_public(req) if req else None,
                "history": [_req_public(r) for r in state.flash_history[-5:]]}

    @app.get("/api/flash/request/{rid}/image")
    def api_flash_request_image(rid: str):
        req = state.flash_request
        if not req or req.get("id") != rid:
            raise HTTPException(status_code=404, detail="冇呢個請求")
        p = Path(req["path"])
        if not p.exists():
            raise HTTPException(status_code=410, detail="映像檔已經清走")
        return Response(content=p.read_bytes(), media_type="application/octet-stream")

    @app.post("/api/flash/request/{rid}/claim")
    def api_flash_request_claim(rid: str, x_actor: str = Header("")):
        """UI 表示「我接手燒」——之後人等瀏覽器彈出選埠。"""
        with state._admin:
            req = state.flash_request
            if not req or req.get("id") != rid:
                raise HTTPException(status_code=404, detail="冇呢個請求")
            if req["status"] != "pending":
                raise HTTPException(status_code=409, detail=f"狀態係 {req['status']}")
            req["status"] = "claimed"
            req["claimedAt"] = time.time()
        state.activity(x_actor or "ui", "flash/claim", id=rid)
        state.broadcast({"type": "flash_request", **_req_public(req)})
        return {"ok": True, "data": _req_public(req)}

    @app.post("/api/flash/request/{rid}/result")
    def api_flash_request_result(rid: str, body: dict = Body(default={}),
                                 x_actor: str = Header("")):
        """UI 燒完回報結果，AI 之後用 GET /api/flash/request 讀。"""
        with state._admin:
            req = state.flash_request
            if not req or req.get("id") != rid:
                raise HTTPException(status_code=404, detail="冇呢個請求")
            ok = bool(body.get("ok"))
            req["status"] = "done" if ok else "failed"
            req["error"] = None if ok else str(body.get("error") or "未知錯誤")[:400]
            req["finishedAt"] = time.time()
            state.flash_history.append(req)
            state.flash_request = None
            try:
                Path(req["path"]).unlink(missing_ok=True)   # 燒完即刪，唔留垃圾
            except OSError:
                pass
        state.activity(x_actor or "ui", "flash/result",
                       id=rid, ok=ok, error=req.get("error"))
        state.broadcast({"type": "flash_result", **_req_public(req)})
        return {"ok": True, "data": _req_public(req)}

    @app.post("/api/flash/request/{rid}/cancel")
    def api_flash_request_cancel(rid: str, x_actor: str = Header("")):
        with state._admin:
            req = state.flash_request
            if not req or req.get("id") != rid:
                raise HTTPException(status_code=404, detail="冇呢個請求")
            req["status"] = "cancelled"
            req["finishedAt"] = time.time()
            state.flash_history.append(req)
            state.flash_request = None
        try:
            Path(req["path"]).unlink(missing_ok=True)
        except OSError:
            pass
        state.activity(x_actor or "api", "flash/cancel", id=rid)
        state.broadcast({"type": "flash_result", **_req_public(req)})
        return {"ok": True, "data": _req_public(req)}

    # ── 格式化（server 係唯一真相；UI 用佢的結果）──
    @app.post("/api/format")
    def api_format(body: dict = Body(...), x_actor: str = Header("")):
        path = str(body.get("path") or "")
        text = body.get("text")
        if not isinstance(text, str):
            raise HTTPException(status_code=400, detail="text 必須係字串")
        try:
            res = formatting.format_text(path, text,
                                         indent=int(body.get("indent", 2)),
                                         sort_keys=bool(body.get("sort_keys")))
        except formatting.FormatError as e:
            return {"ok": False, "detail": {"msg": str(e), "line": e.line, "col": e.col}}
        state.activity(x_actor or "api", "format",
                       path=path, changed=res["changed"], ok=True)
        return {"ok": True, "data": res}

    @app.post("/api/cmd")
    def api_cmd(body: dict = Body(...), x_actor: str = Header("")):
        link = state.require()
        try:
            cmd = int(str(body.get("cmd", "0")), 0)
            payload = bytes.fromhex(str(body.get("payload_hex", "") or ""))
            expect = body.get("expect")
            exp = {int(str(x), 0) for x in expect} if expect else None
            timeout = float(body.get("timeout", 5))
        except (ValueError, TypeError) as e:
            raise HTTPException(status_code=400, detail=f"參數錯：{e}")
        with state.lock:
            try:
                rcmd, rsp = link.request(cmd, payload, expect=exp, timeout=timeout)
            except LinkError as e:
                state.activity(x_actor or "api", "NC4 cmd",
                               cmd=f"0x{cmd:04X}", bytes=len(payload), ok=False)
                raise HTTPException(status_code=504, detail=str(e))
        state.activity(x_actor or "api", "NC4 cmd",
                       cmd=f"0x{cmd:04X}", bytes=len(payload), rsp=f"0x{rcmd:04X}", ok=True)
        return {"ok": True, "cmd": f"0x{rcmd:04X}", "hex": rsp.hex(),
                "json": _json_or_none(rsp), "len": len(rsp)}

    # ── 本機隨身碟（MSC 捷徑）──
    @app.get("/api/drives")
    def api_drives():
        return {"ok": True, "data": _list_drives()}

    @app.get("/api/drive/list")
    def api_drive_list(root: str = Query(...), path: str = "/"):
        r, p = _drive_paths(root, path)
        if not p.is_dir():
            raise HTTPException(status_code=400, detail="唔係資料夾")
        entries = []
        try:
            children = sorted(p.iterdir(), key=lambda x: (not x.is_dir(), x.name.lower()))
        except OSError as e:
            raise HTTPException(status_code=400, detail=f"讀唔到：{e}")
        for child in children:
            try:
                st = child.stat()
            except OSError:
                continue
            entries.append({
                "n": child.name,
                "d": child.is_dir(),
                "s": 0 if child.is_dir() else st.st_size,
                "m": int(st.st_mtime),
            })
        return {"ok": True, "data": {"root": str(r), "path": _rel(r, p), "entries": entries}}

    @app.get("/api/drive/file")
    def api_drive_file_get(root: str = Query(...), path: str = Query(...)):
        r, p = _drive_paths(root, path)
        if not p.is_file():
            raise HTTPException(status_code=404, detail="檔案唔存在")
        try:
            data = p.read_bytes()
        except OSError as e:
            raise HTTPException(status_code=400, detail=f"讀唔到：{e}")
        return Response(content=data, media_type="application/octet-stream",
                        headers={"Content-Disposition": f'attachment; filename="{p.name}"'})

    @app.put("/api/drive/file")
    def api_drive_file_put(root: str = Query(...), path: str = Query(...),
                           body: bytes = Body(...)):
        r, p = _drive_paths(root, path)
        if p.is_dir():
            raise HTTPException(status_code=400, detail="目標係資料夾")
        if len(body) > 16 * 1024 * 1024:
            raise HTTPException(status_code=413, detail="檔案太大（>16 MiB）")
        try:
            p.parent.mkdir(parents=True, exist_ok=True)
            tmp = p.with_name(p.name + ".esptmp")
            tmp.write_bytes(body)
            os.replace(tmp, p)
        except OSError as e:
            raise HTTPException(status_code=400, detail=f"寫唔到：{e}")
        return {"ok": True, "data": {"path": _rel(r, p), "size": len(body)}}

    @app.post("/api/drive/delete")
    def api_drive_delete(body: dict = Body(...)):
        root = str(body.get("root") or "")
        rel = str(body.get("path") or "")
        r, p = _drive_paths(root, rel)
        if p == r:
            raise HTTPException(status_code=400, detail="唔准刪 root")
        try:
            if p.is_dir():
                if not body.get("recursive"):
                    raise HTTPException(status_code=400, detail="資料夾要 recursive=1")
                shutil.rmtree(p)
            else:
                p.unlink(missing_ok=True)
        except OSError as e:
            raise HTTPException(status_code=400, detail=f"刪唔到：{e}")
        return {"ok": True, "data": {"path": rel}}

    @app.post("/api/drive/mkdir")
    def api_drive_mkdir(body: dict = Body(...)):
        root = str(body.get("root") or "")
        rel = str(body.get("path") or "")
        r, p = _drive_paths(root, rel)
        try:
            p.mkdir()
        except FileExistsError:
            pass
        except OSError as e:
            raise HTTPException(status_code=400, detail=f"建唔到：{e}")
        return {"ok": True, "data": {"path": _rel(r, p)}}

    # ── WS 終端 ──
    @app.websocket("/ws/log")
    async def ws_log(ws: WebSocket):
        await ws.accept()
        state.loop = asyncio.get_running_loop()
        state.clients.add(ws)
        await ws.send_json({"type": "hello", "connected": bool(
            state.link and state.link.connected), "port": state.port, "sim": state.is_sim,
            "session": state.session, "device": state.active,
            "devices": state.device_list()})
        # 後連上的人先收 backlog；AI 也用 REST 讀同一份 ring。
        backlog = state.events.tail(300)
        if backlog:
            await ws.send_json({"type": "backlog", "events": backlog})
        try:
            while True:
                await ws.receive_text()
        except WebSocketDisconnect:
            pass
        except Exception:
            pass
        finally:
            state.clients.discard(ws)

    # ── 靜態 SPA ──
    if STATIC_DIR.is_dir():
        app.mount("/static", StaticFiles(directory=STATIC_DIR), name="static")

    @app.get("/")
    def index():
        f = STATIC_DIR / "index.html"
        if f.is_file():
            return FileResponse(f)
        return JSONResponse({"ok": False, "err": "static/index.html 缺失"}, status_code=500)

    if autoconnect:
        try:
            _connect(state, {"sim": True} if autoconnect == "sim" else {"port": autoconnect})
        except Exception as e:                      # 開機自動連線失敗唔應該阻止 UI
            msg = _friendly_connect_error(autoconnect, e)
            state.autoconnect_error = msg
            print(f"[esp_webmaster] {msg}")

    app.state.esp = state        # 測試 / 除錯用：直接攞到 State
    return app


def _simple_file_op(state: State, fn, path: str, actor: str = "", deleted: bool = False):
    if not path:
        raise HTTPException(status_code=400, detail="缺 path")
    link = state.require()
    try:
        with state.lock:
            q = fn(link, path)
    except files.FileError as e:
        raise HTTPException(status_code=400, detail={"kind": e.kind, "msg": str(e)})
    except LinkError as e:
        raise HTTPException(status_code=504, detail=str(e))
    state.broadcast({"type": "file_changed", "path": q.get("path", path),
                     "actor": actor or "api", "size": q.get("size", 0),
                     "deleted": deleted})
    return {"ok": True, "data": {
        "path": q["path"], "exists": q["exists"], "size": q["size"],
        "sha256": q["sha256"].hex(), "pending": q["pending"], "free": q["free"],
    }}


# ── 本機隨身碟（MSC 捷徑）────────────────────────────────
# C 版 firmware 的官方做法：檔案上傳走 MSC，不需要 0x20xx。
# UI 只係讀寫「主機掛載咗嘅碟」；FAT 擁有權仍由 0x1103 file_mode 控制。

def _list_drives() -> list[dict]:
    out: list[dict] = []
    if os.name == "nt":
        for c in "CDEFGHIJKLMNOPQRSTUVWXYZ":
            p = Path(f"{c}:\\")
            try:
                if p.exists():
                    out.append({"path": str(p), "name": f"{c}:"})
            except OSError:
                pass
    elif sys.platform == "darwin":
        base = Path("/Volumes")
        if base.is_dir():
            for p in sorted(base.iterdir()):
                if p.is_dir() and not p.name.startswith("."):
                    out.append({"path": str(p), "name": p.name})
    else:
        for base in (Path("/media"), Path("/mnt"), Path("/run/media")):
            if base.is_dir():
                for p in sorted(base.iterdir()):
                    if p.is_dir():
                        out.append({"path": str(p), "name": p.name})
    return out


def _drive_paths(root: str, rel: str) -> tuple[Path, Path]:
    r = Path(root).expanduser().resolve()
    if not r.is_dir():
        raise HTTPException(status_code=400, detail="root 唔存在或唔係資料夾")
    s = str(rel or "/").replace("\\", "/").lstrip("/")
    parts = [x for x in s.split("/") if x and x != "."]
    if ".." in parts:
        raise HTTPException(status_code=400, detail="路徑唔准有 ..")
    p = (r.joinpath(*parts) if parts else r).resolve()
    try:
        p.relative_to(r)
    except ValueError:
        raise HTTPException(status_code=400, detail="路徑走出 root")
    return r, p


def _rel(root: Path, p: Path) -> str:
    return "/" if p == root else "/" + p.relative_to(root).as_posix()
