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
import shutil
import struct
import sys
import threading
import time
from pathlib import Path
from typing import Any

from fastapi import Body, FastAPI, HTTPException, Query, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse, JSONResponse, Response
from fastapi.staticfiles import StaticFiles

import files
import nc4
import ota
from link import LinkError, SerialLink, SimLink, list_ports
from sim import SimDevice

BASE_DIR = Path(__file__).resolve().parent
STATIC_DIR = BASE_DIR / "static"

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


class State:
    def __init__(self, sim_root: Path | None):
        self.lock = threading.RLock()
        self.link = None
        self.port: str | None = None
        self.is_sim = False
        self.sim_root = sim_root or (BASE_DIR / ".simfs")
        self.loop: asyncio.AbstractEventLoop | None = None
        self.clients: set[WebSocket] = set()

    def broadcast(self, ev: dict[str, Any]) -> None:
        if self.loop is None or not self.clients:
            return

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


def _connect(state: State, body: dict) -> dict:
    with state.lock:
        if state.link is not None:
            try:
                state.link.close()
            except Exception:
                pass
            state.link = None
        if body.get("sim"):
            dev = SimDevice(state.sim_root)
            link = SimLink(dev)
            state.is_sim = True
            state.port = "sim"
        else:
            port = str(body.get("port") or "").strip()
            if not port:
                raise HTTPException(status_code=400, detail="缺 port")
            try:
                link = SerialLink(port)
            except LinkError as e:
                raise HTTPException(status_code=400, detail=str(e))
            state.is_sim = False
            state.port = port

        link.on_line = lambda text: state.broadcast(
            {"type": "line", "text": text, "ts": time.time()})
        link.on_frame = lambda cmd, payload: state.broadcast(
            {"type": "frame", "cmd": f"0x{cmd:04X}", "hex": payload.hex()})
        link.on_close = lambda reason: state.broadcast(
            {"type": "disconnected", "reason": reason})
        state.link = link
    return {"ok": True, "port": state.port, "sim": state.is_sim}


def create_app(sim_root: str | Path | None = None,
               autoconnect: str | None = None) -> FastAPI:
    app = FastAPI(title="ESP WebMaster", version="1.0.0")
    state = State(Path(sim_root) if sim_root else None)

    # ── 連線 ──
    @app.get("/api/ports")
    def api_ports():
        return {"ok": True, "data": list_ports()}

    @app.get("/api/info")
    def api_info():
        return {
            "ok": True,
            "connected": bool(state.link and state.link.connected),
            "port": state.port,
            "sim": state.is_sim,
            "version": "1.0.0",
        }

    @app.post("/api/connect")
    def api_connect(body: dict = Body(...)):
        return _connect(state, body)

    @app.post("/api/disconnect")
    def api_disconnect():
        with state.lock:
            if state.link is not None:
                try:
                    state.link.close()
                except Exception:
                    pass
                state.link = None
            state.port = None
        return {"ok": True}

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

    @app.put("/api/file")
    def api_file_put(path: str = Query(...), body: bytes = Body(...)):
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
        return {
            "ok": True, "path": q["path"], "size": q["size"],
            "sha256": q["sha256"].hex(), "pending": q["pending"], "free": q["free"],
            "auto_confirmed": auto_confirmed,
        }

    @app.post("/api/file/delete")
    def api_file_delete(body: dict = Body(...)):
        return _simple_file_op(state, files.delete, body.get("path", ""))

    @app.post("/api/file/confirm")
    def api_file_confirm(body: dict = Body(...)):
        return _simple_file_op(state, files.confirm, body.get("path", ""))

    @app.post("/api/file/undo")
    def api_file_undo(body: dict = Body(...)):
        return _simple_file_op(state, files.undo, body.get("path", ""))

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

    @app.post("/api/cmd")
    def api_cmd(body: dict = Body(...)):
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
                raise HTTPException(status_code=504, detail=str(e))
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
            state.link and state.link.connected), "port": state.port, "sim": state.is_sim})
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
            print(f"[esp_webmaster] autoconnect 失敗：{e}")

    app.state.esp = state        # 測試 / 除錯用：直接攞到 State
    return app


def _simple_file_op(state: State, fn, path: str):
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
