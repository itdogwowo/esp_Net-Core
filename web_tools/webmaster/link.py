#!/usr/bin/env python3
"""一條 NC4 連線（USB CDC 或模擬裝置）。

責任邊界：
  - 只負責「位元組 ↔ 幀」、請求／回應配對、log 行分流。
  - 唔知檔案、唔知 HTTP、唔知 UI。

request() 的規則：
  1. 同一個 Link 一次只准一個 request（NC4 係一條線，請求序列化）。
  2. expect 係一組 cmd id；收到唔 match 的幀照樣派 on_frame（log 終端要睇到）。
  3. timeout 拋 LinkError；caller 自己決定重試。

為什麼 reader 要用 in_waiting 輪詢：
  pyserial 的 read(N) 在不足 N bytes 時會等滿 timeout 才返回；read(1) 又會阻塞。
  所以「有幾多讀幾多，冇就 sleep 2 ms」——幀一到就即刻交給 Demux。
"""
from __future__ import annotations

import queue
import threading
import time
from typing import Callable, Iterable

import nc4


class LinkError(RuntimeError):
    """連線／協議層錯誤（timeout、斷線、寫入失敗）。"""


class _Waiter:
    __slots__ = ("expect", "q")

    def __init__(self, expect: set[int] | None):
        self.expect = expect
        self.q: queue.Queue = queue.Queue(maxsize=8)


class BaseLink:
    """SerialLink / SimLink 的共用請求／派發邏輯。"""

    def __init__(self) -> None:
        self.on_line: Callable[[str], None] | None = None
        self.on_frame: Callable[[int, bytes], None] | None = None
        self.on_close: Callable[[str], None] | None = None
        self._waiters: list[_Waiter] = []
        self._waiters_lock = threading.Lock()
        self._req_lock = threading.Lock()

    # ── 子類要實作 ──
    def send(self, cmd: int, payload: bytes = b"") -> None:
        raise NotImplementedError

    def close(self) -> None:
        raise NotImplementedError

    @property
    def connected(self) -> bool:
        raise NotImplementedError

    # ── 共用 ──
    def _dispatch(self, events) -> None:
        for kind, value in events:
            if kind == "line":
                if self.on_line is not None:
                    self.on_line(value)
                continue
            cmd, payload = value
            if self.on_frame is not None:
                self.on_frame(cmd, payload)
            with self._waiters_lock:
                waiters = list(self._waiters)
            for w in waiters:
                if w.expect is None or cmd in w.expect:
                    try:
                        w.q.put_nowait((cmd, payload))
                    except queue.Full:
                        pass

    def request(self, cmd: int, payload: bytes = b"",
                expect: Iterable[int] | None = None,
                timeout: float = 3.0) -> tuple[int, bytes]:
        """送一幀並等回覆。expect=None = 任何幀都收。"""
        w = _Waiter(set(expect) if expect is not None else None)
        with self._req_lock:
            if not self.connected:
                raise LinkError("not connected")
            with self._waiters_lock:
                self._waiters.append(w)
            try:
                self.send(cmd, payload)
                deadline = time.monotonic() + timeout
                while True:
                    remain = deadline - time.monotonic()
                    if remain <= 0:
                        raise LinkError(
                            f"timeout waiting 0x{cmd:04X} → "
                            f"{sorted(w.expect) if w.expect else 'any'}"
                        )
                    try:
                        return w.q.get(timeout=remain)
                    except queue.Empty:
                        continue
            finally:
                with self._waiters_lock:
                    if w in self._waiters:
                        self._waiters.remove(w)


class SerialLink(BaseLink):
    """USB CDC（pyserial）。開埠即起 reader thread。"""

    def __init__(self, port: str, baud: int = 115200) -> None:
        super().__init__()
        try:
            import serial
        except ImportError as e:  # pragma: no cover - 環境問題
            raise LinkError("需要 pyserial：pip install pyserial") from e

        self.port = port
        try:
            self._ser = serial.Serial(port, baud, timeout=0.05, write_timeout=3.0)
        except Exception as e:
            raise LinkError(f"打唔開 {port}：{e}") from e

        # TinyUSB CDC 要 host 拉高 DTR 才會收／送資料。
        try:
            self._ser.dtr = True
            self._ser.rts = True
        except Exception:
            pass

        self._demux = nc4.Demux()
        self._stop = threading.Event()
        self._write_lock = threading.Lock()
        self._closed = False
        self._thread = threading.Thread(target=self._reader, name="nc4-reader", daemon=True)
        self._thread.start()
        time.sleep(0.15)          # 等 DTR 穩定

    @property
    def connected(self) -> bool:
        return not self._closed and self._ser is not None and self._ser.is_open

    def send(self, cmd: int, payload: bytes = b"") -> None:
        if not self.connected:
            raise LinkError("not connected")
        frame = nc4.pack(cmd, payload)
        try:
            with self._write_lock:
                self._ser.write(frame)
        except Exception as e:
            self._fail(f"write failed: {e}")
            raise LinkError(f"write failed: {e}") from e

    def _reader(self) -> None:
        try:
            while not self._stop.is_set():
                try:
                    n = self._ser.in_waiting
                except Exception as e:
                    self._fail(f"serial lost: {e}")
                    return
                if n == 0:
                    time.sleep(0.002)
                    continue
                try:
                    data = self._ser.read(n)
                except Exception as e:
                    self._fail(f"serial lost: {e}")
                    return
                if data:
                    self._dispatch(self._demux.feed(data))
        finally:
            pass

    def _fail(self, reason: str) -> None:
        if self._closed:
            return
        self._closed = True
        self._stop.set()
        if self.on_close is not None:
            self.on_close(reason)

    def close(self) -> None:
        if self._closed and self._ser is None:
            return
        self._closed = True
        self._stop.set()
        if self._thread.is_alive():
            self._thread.join(timeout=1.0)
        try:
            if self._ser is not None and self._ser.is_open:
                self._ser.close()
        except Exception:
            pass
        self._ser = None


class SimLink(BaseLink):
    """In-process 模擬裝置；介面同 SerialLink 一樣，唔使任何硬件。"""

    def __init__(self, device) -> None:
        super().__init__()
        self.device = device
        self.port = "sim"
        self._closed = False

    @property
    def connected(self) -> bool:
        return not self._closed

    def send(self, cmd: int, payload: bytes = b"") -> None:
        if self._closed:
            raise LinkError("not connected")
        events = self.device.handle(cmd, payload)
        if events:
            self._dispatch(events)

    def close(self) -> None:
        self._closed = True


def list_ports() -> list[dict]:
    """列出本機序列埠；pyserial 缺就回空 list（UI 顯示提示）。"""
    try:
        from serial.tools import list_ports as lp
    except ImportError:
        return []
    out = []
    for p in lp.comports():
        out.append({
            "device": p.device,
            "description": p.description or "",
            "hwid": p.hwid or "",
            "vid": p.vid,
            "pid": p.pid,
        })
    return out


# ── 「真實板子」偵測 ────────────────────────────────────────────────────
# `autoconnect: "auto"` 用呢個揀埠：**只揀 USB 序列（有 VID）**，
# 而且排除明顯唔係板子的東西（Intel AMT、藍牙 SPP、內建 COM1）。
#
# USB VID：
#   0x303A  Espressif 原生 USB（S3/C3/P4 的 USB-Serial-JTAG / TinyUSB CDC）
#   0x10C4  Silicon Labs CP210x（很多 DevKit 的外置 USB-UART）
#   0x1A86  WCH CH340/CH343
#   0x0403  FTDI FT232
#   0x1B4F  SparkFun、0x2341 Arduino（少見但要認）
_ESP_VIDS = {0x303A, 0x10C4, 0x1A86, 0x0403, 0x1B4F, 0x2341}
# 一定唔係板子的描述關鍵字（Windows 上這些也會出現在 comports()）
_NOT_A_BOARD = ("bluetooth", "藍牙", "active management", "amt - sol", "intel(r) amt")


def is_likely_board(port: dict) -> bool:
    """判斷一個序列埠係唔係「疑似 ESP 板」。"""
    desc = f"{port.get('description') or ''} {port.get('hwid') or ''}".lower()
    if any(bad in desc for bad in _NOT_A_BOARD):
        return False
    vid = port.get("vid")
    if vid is None:
        return False          # 冇 VID＝唔係 USB 序列（PCI／藍牙／虛擬埠）
    if vid in _ESP_VIDS:
        return True
    # 未知 VID 但描述提到 ESP／USB Serial：當係候選
    return "esp" in desc or "usb serial" in desc or "usb-serial" in desc


def _skip_reason(port: dict) -> str:
    """解釋「點解唔當係板」（測試同 log 用，唔好靜靜跳過）。"""
    desc = f"{port.get('description') or ''} {port.get('hwid') or ''}".lower()
    if "bluetooth" in desc or "藍牙" in desc:
        return "藍牙序列埠"
    if "active management" in desc or "amt" in desc:
        return "Intel AMT SOL"
    if port.get("vid") is None:
        return "非 USB 序列（冇 VID）"
    return "USB 序列但唔係已知板 VID"


def _vid_label(port: dict) -> str:
    vid, pid = port.get("vid"), port.get("pid")
    if vid is None:
        return "?"
    name = {0x303A: "Espressif", 0x10C4: "CP210x", 0x1A86: "CH34x",
            0x0403: "FTDI", 0x1B4F: "SparkFun", 0x2341: "Arduino"}.get(vid)
    code = f"{vid:04X}" + (f":{pid:04X}" if pid is not None else "")
    return f"{name} {code}" if name else code


def scan_ports(prefer: str | None = None) -> dict:
    """掃一次序列埠並分類 → 俾啟動訊息／`--list-ports` 用。

    只讀 `comports()` 的**中繼資料**（VID／PID／description），
    **唔會逐個開埠試**——開埠會打斷板上正在跑的程式，也可能搶到別人的連線。
    真正連線只會開最後揀中的那一個。
    """
    ports = list_ports()
    boards = [p for p in ports if is_likely_board(p)]
    skipped = [{"device": p.get("device") or "", "reason": _skip_reason(p)}
               for p in ports if not is_likely_board(p)]
    boards_sorted = sorted(boards, key=lambda p: (0 if p.get("vid") == 0x303A else 1,
                                                  str(p.get("device") or "")))
    chosen = None
    if prefer:
        chosen = next((b for b in boards_sorted if b.get("device") == prefer), None)
        if chosen is None:
            chosen = find_board_port(prefer=prefer)
    elif boards_sorted:
        chosen = boards_sorted[0]
    others = [b for b in boards_sorted if b is not chosen]
    return {
        "total": len(ports),
        "boards": [{"device": b.get("device"), "vid": _vid_label(b),
                    "description": b.get("description") or ""} for b in boards_sorted],
        "chosen": chosen.get("device") if chosen else None,
        "chosenVid": _vid_label(chosen) if chosen else None,
        "others": [{"device": b.get("device"), "vid": _vid_label(b)} for b in others],
        "skipped": skipped,
    }


def find_board_port(prefer: str | None = None) -> str | None:
    """揀一個真實板子的序列埠；冇就回 None。

    `prefer`（設定檔的 device.autoconnect 可以是具體埠名）會先試。
    排序：Espressif 原生 USB 優先（303A），其後其他 VID；同分按埠名。
    """
    ports = [p for p in list_ports() if is_likely_board(p)]
    if prefer and any(p["device"] == prefer for p in ports):
        return prefer
    if not ports:
        return None
    ports.sort(key=lambda p: (0 if p.get("vid") == 0x303A else 1,
                              str(p.get("device") or "")))
    return ports[0]["device"]


def format_scan(scan: dict) -> list[str]:
    """把 `scan_ports()` 的結果變成幾行人話（啟動訊息／CLI 共用）。

    刻意只用 ASCII 符號（唔用 ✓／·）：Windows 主控台係 cp950，
    呢類符號唔喺 Big5 裡面，一印就 UnicodeEncodeError。
    """
    lines = [f"掃描序列埠：共 {scan['total']} 個 -> 疑似板 {len(scan['boards'])} 個"
             "（只讀 USB 中繼資料，唔會逐個開埠試）"]
    if scan["chosen"]:
        lines.append(f"   選中 {scan['chosen']}（{scan['chosenVid']}）")
    for o in scan["others"]:
        lines.append(f"   另一個板 {o['device']}（{o['vid']}）"
                     f"—— 要指定就 --serial {o['device']}")
    if scan["skipped"]:
        shown = scan["skipped"][:6]
        txt = "、".join(f"{s['device']}={s['reason']}" for s in shown)
        more = "" if len(scan["skipped"]) <= 6 else f" 等 {len(scan['skipped'])} 個"
        lines.append(f"   跳過{more}：{txt}")
    if not scan["boards"]:
        lines.append("   （冇疑似板：確認 USB 線插好、板有電、驅動正常）")
    return lines
