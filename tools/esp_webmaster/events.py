#!/usr/bin/env python3
"""共享事件流：device log／frame／activity 一份，Web UI 與 AI 都讀同一份。

為什麼要這個：
  - 以前 log 只經 WebSocket 即時廣播；後來連上的人看不到之前的行，
    AI 也無法「讀最近 200 行」。
  - 序列埠由 server 獨佔；AI 唔應該再開第二條連線（會搶 port）。
    所以 server 把「看到什麼、誰做了什麼」寫入同一個 ring，對外一次過提供：
      GET /api/log/tail?n=…&since=…
      GET /api/activity?n=…
      WS /ws/log（連上先補 backlog，之後即時）
"""
from __future__ import annotations

import threading
import time
from collections import deque
from typing import Any


class EventLog:
    def __init__(self, limit: int = 4000) -> None:
        self._events: deque[dict[str, Any]] = deque(maxlen=limit)
        self._lock = threading.Lock()
        self._seq = 0

    def add(self, kind: str, text: str = "", **data: Any) -> dict[str, Any]:
        with self._lock:
            self._seq += 1
            ev: dict[str, Any] = {
                "seq": self._seq,
                "ts": time.time(),
                "type": kind,
            }
            if text:
                ev["text"] = text
            ev.update(data)
            self._events.append(ev)
            return ev

    def tail(self, n: int = 200, since: int = 0,
             kind: str | None = None) -> list[dict[str, Any]]:
        n = max(1, min(int(n), 2000))
        with self._lock:
            items = list(self._events)
        out = [e for e in items if int(e.get("seq", 0)) > since]
        if kind:
            out = [e for e in out if e.get("type") == kind]
        return out[-n:]
