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
