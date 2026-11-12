#!/usr/bin/env python3
"""模擬 ESP 裝置：同一個 NC4 介面、真檔案系統（host 上一個暫存目錄）。

用途：
  1. 冇板都可以完整試 UI（上傳／下載／編輯／confirm／undo／OTA）。
  2. 協議測試唔使硬件、唔會傷 flash。

行為刻意同 firmware 對齊：
  - /config/system.json 之類 → root/config/system.json
  - 上傳先寫 `<path>.part`，END 驗 sha 才 rename；舊檔留 `<path>.bak`
  - pending = `.bak` 存在
  - 0x2010 七個 bool + failed_offset + written_up_to + path
  - BEGIN 成功回 0x2004（file_id, offset=0）；失敗回 0x2010
  - FILE_SCAN 寫 /manifest.json（跟 mp：{path:{"s","h","p"}}）

注意：模擬器唔會假裝有 Wi-Fi／SD；storage 只報 flash 分區。
"""
from __future__ import annotations

import hashlib
import json
import shutil
import struct
import threading
import time
from pathlib import Path

TOTAL_BYTES = 7 * 1024 * 1024

# 指令碼
CMD_REBOOT = 0x100F
CMD_STATUS_GET = 0x1101
CMD_STATUS_RSP = 0x1102
CMD_STATUS_SET = 0x1103
CMD_STATUS_ACK = 0x1104
CMD_FILE_BEGIN = 0x2001
CMD_FILE_CHUNK = 0x2002
CMD_FILE_END = 0x2003
CMD_FILE_ACK = 0x2004
CMD_FILE_QUERY = 0x2005
CMD_FILE_QUERY_RSP = 0x2006
CMD_FILE_READ = 0x2007
CMD_FILE_CONFIRM = 0x2008
CMD_FILE_DELETE = 0x2009
CMD_FILE_UNDO = 0x200A
CMD_FILE_SCAN = 0x200B
CMD_FILE_ERROR_RSP = 0x2010
CMD_OTA_BEGIN = 0x2201
CMD_OTA_WRITE = 0x2202
CMD_OTA_END = 0x2203
CMD_OTA_ACK = 0x2204
CMD_OTA_CAPS_QUERY = 0x2210
CMD_OTA_CAPS_RSP = 0x2211
CMD_OTA_STATE_RSP = 0x221B
CMD_OTA_ERROR_RSP = 0x221C
CMD_OTA_APPLY = 0x2220

ERR_FIELDS = [
    "err_no_space", "err_write_fail", "err_offset_mismatch", "err_id_mismatch",
    "err_sha_mismatch", "err_not_active", "err_busy",
]

SEED = {
    "config/system.json": json.dumps({
        "name": "ESP-SIM",
        "usb": {"msc": 1, "cdc_log": 1},
        "log": {"level": "I"},
    }, ensure_ascii=False, indent=2),
    "config/network.json": json.dumps({
        "wifi": {"ssid": "sim-ap", "mode": "sta"},
        "net": {"master_port": 8000},
    }, ensure_ascii=False, indent=2),
    "config/hardware.json": json.dumps({
        "wiring": {"led": {"gpio": 48, "count": 64}},
        "pixel": {"fps": 40},
    }, ensure_ascii=False, indent=2),
    "pixel/effects/breathe.json": json.dumps({
        "name": "breathe", "fps": 40, "colors": ["#38bdf8", "#34d399"],
    }, ensure_ascii=False, indent=2),
    "pixel/effects/demo.json": json.dumps({
        "name": "demo", "fps": 30, "zones": [0, 1, 2],
    }, ensure_ascii=False, indent=2),
    "README.txt": "ESP WebMaster 模擬裝置\n可以直接拖拉檔案上來試。\n",
}


class SimDevice:
    def __init__(self, root: str | Path):
        self.root = Path(root)
        self.root.mkdir(parents=True, exist_ok=True)
        self._lock = threading.RLock()
        self._mode = "app"                 # app | usb
        self._boot = time.time()
        self._format_pending = False
        self._session: dict | None = None
        self._ota: dict | None = None
        self._log_seq = 0
        self._seed_if_empty()

    # ── 基本 ──
    def _seed_if_empty(self) -> None:
        for rel, text in SEED.items():
            p = self.root / rel
            if not p.exists():
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text(text, encoding="utf-8")

    def _path(self, req: str) -> Path | None:
        if req is None:
            return None
        s = str(req).replace("\\", "/").strip()
        if s.startswith("/"):
            s = s[1:]
        if ".." in s.split("/"):
            return None
        p = (self.root / s).resolve() if s else self.root.resolve()
        try:
            p.relative_to(self.root.resolve())
        except ValueError:
            return None
        return p

    def _used(self) -> int:
        total = 0
        for p in self.root.rglob("*"):
            if p.is_file():
                try:
                    total += p.stat().st_size
                except OSError:
                    pass
        return total

    def _free(self) -> int:
        return max(0, TOTAL_BYTES - self._used())

    # ── manifest（跟 mp：{path: {"s","h","p"}}）──
    def _manifest_file(self) -> Path:
        return self.root / "manifest.json"

    def _manifest_load(self) -> dict:
        try:
            obj = json.loads(self._manifest_file().read_text(encoding="utf-8"))
            return obj if isinstance(obj, dict) else {}
        except (OSError, json.JSONDecodeError):
            return {}

    def _manifest_save(self, obj: dict) -> None:
        p = self._manifest_file()
        tmp = p.with_suffix(".json.tmp")
        tmp.write_text(json.dumps(obj, ensure_ascii=False, separators=(",", ":")),
                       encoding="utf-8")
        tmp.replace(p)

    def _manifest_set(self, path: str, size: int, sha: bytes, pending: bool) -> None:
        obj = self._manifest_load()
        obj[path] = {"s": int(size), "h": sha.hex(), "p": 1 if pending else 0}
        self._manifest_save(obj)

    def _manifest_pending(self, path: str, pending: bool) -> None:
        obj = self._manifest_load()
        if path in obj and isinstance(obj[path], dict):
            obj[path]["p"] = 1 if pending else 0
            self._manifest_save(obj)

    def _manifest_remove(self, path: str) -> None:
        obj = self._manifest_load()
        if path in obj:
            del obj[path]
            self._manifest_save(obj)

    def _line(self, level: str, msg: str) -> tuple:
        self._log_seq += 1
        return ("line", f"{level} ({self._log_seq}) sim: {msg}")

    def _frame(self, cmd: int, payload: bytes = b"") -> tuple:
        return ("frame", (cmd, payload))

    # ── 回覆工具 ──
    def _query_rsp(self, req: str) -> bytes:
        p = self._path(req)
        exists, size, sha, bak = 0, 0, b"\x00" * 32, False
        if p is not None and p.is_file():
            exists = 1
            data = p.read_bytes()
            size = len(data)
            sha = hashlib.sha256(data).digest()
            bak = Path(str(p) + ".bak").exists()
        payload = struct.pack("<B", exists) + sha + struct.pack("<I", size)
        payload += _str16(req or "/") + struct.pack("<I", self._free())
        payload += struct.pack("<B", 1 if bak else 0)
        return self._frame(CMD_FILE_QUERY_RSP, payload)

    def _error(self, kind: str, path: str = "", written: int = 0,
               failed: int = 0) -> tuple:
        flags = [0] * len(ERR_FIELDS)
        if kind in ERR_FIELDS:
            flags[ERR_FIELDS.index(kind)] = 1
        payload = bytes(flags) + struct.pack("<II", failed, written) + _str16(path)
        return self._frame(CMD_FILE_ERROR_RSP, payload)

    def _ok_query(self, path: str, msg: str) -> list:
        return [self._line("I", msg), self._query_rsp(path)]

    # ── 入口 ──
    def handle(self, cmd: int, payload: bytes) -> list[tuple]:
        with self._lock:
            try:
                return self._handle(cmd, payload)
            except Exception as e:  # 模擬器永遠唔應該搞死連線
                return [self._line("E", f"handler 0x{cmd:04X} 例外：{e}")]

    def _handle(self, cmd: int, payload: bytes) -> list[tuple]:
        if cmd == CMD_STATUS_GET:
            return self._status(payload)
        if cmd == CMD_STATUS_SET:
            return self._status_set(payload)
        if cmd == CMD_REBOOT:
            return [self._line("I", "REBOOT 收到（模擬：唔會真重啟）")]
        if cmd == CMD_FILE_QUERY:
            req = _decode_str16(payload)
            return [self._line("I", f"FILE_QUERY {req}"), self._query_rsp(req or "")]
        if cmd == CMD_FILE_BEGIN:
            return self._file_begin(payload)
        if cmd == CMD_FILE_CHUNK:
            return self._file_chunk(payload)
        if cmd == CMD_FILE_END:
            return self._file_end(payload)
        if cmd == CMD_FILE_READ:
            return self._file_read(payload)
        if cmd == CMD_FILE_CONFIRM:
            return self._file_confirm(payload)
        if cmd == CMD_FILE_DELETE:
            return self._file_delete(payload)
        if cmd == CMD_FILE_UNDO:
            return self._file_undo(payload)
        if cmd == CMD_FILE_SCAN:
            return self._file_scan(payload)
        if cmd == CMD_OTA_CAPS_QUERY:
            return [self._frame(CMD_OTA_CAPS_RSP,
                                struct.pack("<HBBBB", 4096, 0, 0, 0, 0))]
        if cmd == CMD_OTA_BEGIN:
            return self._ota_begin(payload)
        if cmd == CMD_OTA_WRITE:
            return self._ota_write(payload)
        if cmd == CMD_OTA_END:
            return self._ota_end()
        if cmd == CMD_OTA_APPLY:
            return [self._line("I", "OTA APPLY（模擬：寫入 firmware.bin）"),
                    self._frame(CMD_OTA_STATE_RSP, b"\x01\x00\x00\x00" + _str16("sim"))]
        return [self._line("W", f"未知指令 0x{cmd:04X}（{len(payload)} B）")]

    # ── STATUS ──
    def _status(self, payload: bytes) -> list[tuple]:
        keys = _decode_str16(payload) or ""
        want = {k.strip() for k in keys.split(",") if k.strip()}
        pending = sum(1 for p in self.root.rglob("*.bak"))
        allk = {
            "sys": {
                "chip": "esp32-s3", "idf": "v5.5-sim", "fw": "esp-sim-1.0",
                "up_s": int(time.time() - self._boot), "heap": 355_000, "rst": 0,
            },
            "storage": {
                "mode": self._mode, "app_ready": 1 if self._mode == "app" else 0,
                "free": self._free(), "total": TOTAL_BYTES, "pending": pending,
            },
            "log": {
                "cap": 1, "run": 1, "lvl": "D" if self._debug else "I",
                "produced": self._log_seq, "sent": self._log_seq, "dropped": 0,
            },
            "cfg": {"sys": "ok", "net": "ok", "hw": "ok", "unk": 0},
        }
        data = {k: v for k, v in allk.items() if not want or k in want}
        text = json.dumps(data, ensure_ascii=False, separators=(",", ":"))
        # 同 firmware 一樣用 str_u16len 包 JSON（JSON 可以長過路徑，唔用 200 上限）
        return [self._frame(CMD_STATUS_RSP, _json16(text))]

    def _status_set(self, payload: bytes) -> list[tuple]:
        text = _decode_str16(payload) or "{}"
        try:
            obj = json.loads(text)
        except json.JSONDecodeError:
            return [self._frame(CMD_STATUS_ACK, b"\x00" + _str16("bad json"))]
        sys = obj.get("System", {}) if isinstance(obj, dict) else {}
        if "file_mode" in sys:
            self._mode = "app" if int(sys["file_mode"]) else "usb"
            return [self._frame(CMD_STATUS_ACK, b"\x01" + _str16(
                f"file_mode={sys['file_mode']} mode={self._mode}"))]
        if "fs_format" in sys or "format" in sys:
            self._format_pending = bool(int(sys.get("fs_format", sys.get("format", 0))))
            return [self._frame(CMD_STATUS_ACK, b"\x01" + _str16(
                "fs_format=%d（模擬：下次連線重建）" % (1 if self._format_pending else 0)))]
        if "log_debug_mode" in sys:
            self._debug = int(sys["log_debug_mode"]) == 1
            return [self._frame(CMD_STATUS_ACK, b"\x01" + _str16(
                f"log_debug_mode={int(self._debug)}"))]
        return [self._frame(CMD_STATUS_ACK, b"\x00" + _str16(
            "only System.file_mode / fs_format / log_debug_mode"))]

    _debug = False

    # ── FILE 上傳 ──
    def _file_begin(self, payload: bytes) -> list[tuple]:
        if self._mode != "app":
            return [self._error("err_busy", _decode_str16(payload[40:]) or "")]
        if len(payload) < 40:
            return [self._error("err_write_fail")]
        fid, total, chunk = struct.unpack_from("<HIH", payload, 0)
        sha = payload[8:40]
        req = _decode_str16(payload[40:])
        if not req:
            return [self._error("err_write_fail")]
        if self._session is not None:
            return [self._error("err_busy", req)]
        p = self._path(req)
        if p is None:
            return [self._error("err_write_fail", req)]
        if total > self._free():
            return [self._error("err_no_space", req)]
        if chunk < 1 or chunk > 4096:
            return [self._error("err_write_fail", req)]
        p.parent.mkdir(parents=True, exist_ok=True)
        tmp = Path(str(p) + ".part")
        try:
            f = open(tmp, "wb")
        except OSError:
            return [self._error("err_write_fail", req)]
        self._session = {
            "id": fid, "path": req, "full": p, "tmp": tmp, "f": f,
            "size": total, "chunk": chunk, "written": 0,
            "sha": hashlib.sha256(), "sha_expect": sha,
        }
        return [self._line("I", f"FILE_BEGIN {req} ({total} B)"),
                self._frame(CMD_FILE_ACK, struct.pack("<HI", fid, 0))]

    def _file_chunk(self, payload: bytes) -> list[tuple]:
        s = self._session
        if s is None:
            return [self._error("err_not_active")]
        if len(payload) < 6:
            return [self._error("err_write_fail", s["path"])]
        fid, off = struct.unpack_from("<HI", payload, 0)
        data = payload[6:]
        if fid != s["id"]:
            return [self._error("err_id_mismatch", s["path"], s["written"])]
        if off != s["written"]:
            return [self._error("err_offset_mismatch", s["path"], s["written"], off)]
        try:
            s["f"].write(data)
            s["f"].flush()
        except OSError:
            return [self._error("err_write_fail", s["path"], s["written"])]
        s["sha"].update(data)
        s["written"] += len(data)
        return [self._frame(CMD_FILE_ACK, struct.pack("<HI", fid, off))]

    def _file_end(self, payload: bytes) -> list[tuple]:
        s = self._session
        if s is None:
            return [self._error("err_not_active")]
        fid = struct.unpack_from("<H", payload, 0)[0] if len(payload) >= 2 else s["id"]
        if fid != s["id"]:
            return [self._error("err_id_mismatch", s["path"], s["written"])]
        self._session = None
        try:
            s["f"].close()
        except OSError:
            pass
        if s["written"] != s["size"]:
            return [self._error("err_write_fail", s["path"], s["written"])]
        got = s["sha"].digest()
        if got != s["sha_expect"]:
            try:
                s["tmp"].unlink()
            except OSError:
                pass
            return [self._error("err_sha_mismatch", s["path"], s["written"])]
        full: Path = s["full"]
        bak = Path(str(full) + ".bak")
        try:
            if full.exists():
                bak.unlink(missing_ok=True)
                full.rename(bak)
            s["tmp"].rename(full)
        except OSError:
            return [self._error("err_write_fail", s["path"], s["written"])]
        self._manifest_set(s["path"], s["size"], got, bak.exists())
        return self._ok_query(s["path"], f"FILE_END ok {s['path']} ({s['size']} B)")

    # ── FILE 其他 ──
    def _file_read(self, payload: bytes) -> list[tuple]:
        req = _decode_str16(payload)
        if req is None:
            return [self._error("err_write_fail")]
        n = struct.unpack_from("<H", payload, 0)[0]
        if len(payload) < 2 + n + 6:
            return [self._error("err_write_fail", req)]
        off, length = struct.unpack_from("<IH", payload, 2 + n)
        p = self._path(req)
        if p is None or not p.is_file():
            return [self._frame(0x2002, struct.pack("<HI", 0, off))]
        try:
            with open(p, "rb") as f:
                f.seek(off)
                data = f.read(min(int(length), 4096))
        except OSError:
            data = b""
        return [self._frame(0x2002, struct.pack("<HI", 0, off) + data)]

    def _file_confirm(self, payload: bytes) -> list[tuple]:
        req = _decode_str16(payload) or ""
        p = self._path(req)
        if p is not None:
            Path(str(p) + ".bak").unlink(missing_ok=True)
            self._manifest_pending(req, False)
        return self._ok_query(req, f"FILE_CONFIRM {req}")

    def _file_delete(self, payload: bytes) -> list[tuple]:
        req = _decode_str16(payload) or ""
        p = self._path(req)
        if p is not None:
            if p.is_dir():
                shutil.rmtree(p, ignore_errors=True)
            else:
                p.unlink(missing_ok=True)
            Path(str(p) + ".bak").unlink(missing_ok=True)
            Path(str(p) + ".part").unlink(missing_ok=True)
            self._manifest_remove(req)
        return self._ok_query(req, f"FILE_DELETE {req}")

    def _file_undo(self, payload: bytes) -> list[tuple]:
        req = _decode_str16(payload) or ""
        p = self._path(req)
        if p is not None:
            bak = Path(str(p) + ".bak")
            if bak.exists():
                p.unlink(missing_ok=True)
                bak.rename(p)
                data = p.read_bytes()
                self._manifest_set(req, len(data), hashlib.sha256(data).digest(), False)
        return self._ok_query(req, f"FILE_UNDO {req}")

    def _file_scan(self, payload: bytes) -> list[tuple]:
        """0x200B：walk 整個 root，寫 manifest；schema 冇回覆。"""
        target = payload[0] if payload else 0
        if target != 0:
            return [self._line("W", f"FILE_SCAN target={target}：SD 未支援")]
        obj = {}
        for p in sorted(self.root.rglob("*")):
            if not p.is_file():
                continue
            rel = "/" + p.relative_to(self.root).as_posix()
            if rel == "/manifest.json" or p.name.endswith((".bak", ".part", ".tmp")):
                continue
            data = p.read_bytes()
            obj[rel] = {
                "s": len(data),
                "h": hashlib.sha256(data).hexdigest(),
                "p": 1 if Path(str(p) + ".bak").exists() else 0,
            }
            if len(obj) >= 200:
                break
        self._manifest_save(obj)
        return [self._line("I", f"FILE_SCAN {len(obj)} 個檔")]

    # ── OTA ──
    def _ota_begin(self, payload: bytes) -> list[tuple]:
        if len(payload) < 38:
            return [self._frame(CMD_OTA_ERROR_RSP, _ota_err(5))]
        size, chunk = struct.unpack_from("<IH", payload, 0)
        sha = payload[6:38]
        self._ota = {"size": size, "chunk": chunk, "sha": sha,
                     "buf": bytearray(), "done": False}
        return [self._frame(CMD_OTA_ACK, struct.pack("<II", 0, 0))]

    def _ota_write(self, payload: bytes) -> list[tuple]:
        if self._ota is None:
            return [self._frame(CMD_OTA_ERROR_RSP, _ota_err(7))]
        off = struct.unpack_from("<I", payload, 0)[0]
        data = payload[4:]
        if off != len(self._ota["buf"]):
            return [self._frame(CMD_OTA_ERROR_RSP, _ota_err(3, off, len(self._ota["buf"])))]
        self._ota["buf"].extend(data)
        return [self._frame(CMD_OTA_ACK, struct.pack("<II", off, len(self._ota["buf"])))]

    def _ota_end(self) -> list[tuple]:
        o = self._ota
        if o is None:
            return [self._frame(CMD_OTA_ERROR_RSP, _ota_err(7))]
        if len(o["buf"]) != o["size"] or hashlib.sha256(o["buf"]).digest() != o["sha"]:
            return [self._frame(CMD_OTA_ERROR_RSP, _ota_err(6, 0, len(o["buf"])))]
        (self.root / "firmware.bin").write_bytes(bytes(o["buf"]))
        self._ota = None
        return [self._frame(CMD_OTA_STATE_RSP,
                            bytes([0, 0, 1, 0]) + _str16("sim"))]


def _str16(s: str) -> bytes:
    b = (s or "").encode("utf-8")[:200]
    return struct.pack("<H", len(b)) + b


def _json16(s: str, limit: int = 4096) -> bytes:
    b = (s or "").encode("utf-8")[:limit]
    return struct.pack("<H", len(b)) + b


def _decode_str16(payload: bytes) -> str | None:
    if len(payload) < 2:
        return None
    n = struct.unpack_from("<H", payload, 0)[0]
    if 2 + n > len(payload):
        return None
    return payload[2:2 + n].decode("utf-8", "replace")


def _ota_err(idx: int, failed: int = 0, written: int = 0) -> bytes:
    flags = bytearray(11)
    if 0 <= idx < 11:
        flags[idx] = 1
    return bytes(flags) + struct.pack("<II", failed, written) + _str16("sim")
