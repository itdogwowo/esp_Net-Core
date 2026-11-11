#!/usr/bin/env python3
"""NC4 over network 的最小效能測試（TCP/UDP/WS）。

用法：
  python3 tools/net_bench.py --ip 10.10.1.31 --mode tcp|udp|ws --count 50
  python3 tools/net_bench.py --ip ... --mode tcp --count 100 --burst

- 預設是「一問一答」RTT。
- --burst 先全部送出再收（測吞吐／背壓；目前 S3 PULL 版本可能 reset）。
"""
from __future__ import annotations

import argparse
import json
import socket
import statistics
import struct
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from nc4_demux import pack, ADDR_BROADCAST  # noqa: E402

REQ = pack(0x1101, struct.pack("<H", 3) + b"sys", ADDR_BROADCAST)


def frames(buf: bytes):
    out = []
    while len(buf) >= 9:
        dlen = struct.unpack_from("<H", buf, 7)[0]
        total = 9 + dlen + 4
        if len(buf) < total:
            break
        out.append(buf[:total])
        buf = buf[total:]
    return out, buf


def rtt_tcp(ip: str, port: int, n: int, burst: bool):
    s = socket.create_connection((ip, port), timeout=3)
    s.settimeout(3)
    buf = b""
    times = []
    t0 = time.time()
    if burst:
        for _ in range(n):
            s.sendall(REQ)
    got = 0
    while got < n:
        if not burst:
            t = time.time()
            s.sendall(REQ)
        d = s.recv(65536)
        if not d:
            raise RuntimeError("connection closed")
        buf += d
        fs, buf = frames(buf)
        if fs:
            got += len(fs)
            if not burst:
                times.append((time.time() - t) * 1000)
    dt = time.time() - t0
    s.close()
    return times, got, dt


def rtt_udp(ip: str, port: int, n: int, burst: bool):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(3)
    buf = b""
    times = []
    t0 = time.time()
    if burst:
        for _ in range(n):
            s.sendto(REQ, (ip, port))
    got = 0
    while got < n:
        if not burst:
            t = time.time()
            s.sendto(REQ, (ip, port))
        d, _ = s.recvfrom(65536)
        buf += d
        fs, buf = frames(buf)
        if fs:
            got += len(fs)
            if not burst:
                times.append((time.time() - t) * 1000)
    dt = time.time() - t0
    s.close()
    return times, got, dt


def rtt_ws(ip: str, port: int, n: int, burst: bool):
    import websocket  # type: ignore
    ws = websocket.create_connection(f"ws://{ip}:{port}/nc4", timeout=3)
    buf = b""
    times = []
    t0 = time.time()
    if burst:
        for _ in range(n):
            ws.send_binary(REQ)
    got = 0
    while got < n:
        if not burst:
            t = time.time()
            ws.send_binary(REQ)
        d = ws.recv()
        if isinstance(d, str):
            d = d.encode()
        buf += d
        fs, buf = frames(buf)
        if fs:
            got += len(fs)
            if not burst:
                times.append((time.time() - t) * 1000)
    dt = time.time() - t0
    ws.close()
    return times, got, dt


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip", required=True)
    ap.add_argument("--mode", choices=("tcp", "udp", "ws"), default="tcp")
    ap.add_argument("--port", type=int, default=0)
    ap.add_argument("--count", type=int, default=20)
    ap.add_argument("--burst", action="store_true")
    a = ap.parse_args()
    port = a.port or {"tcp": 6000, "udp": 6001, "ws": 8080}[a.mode]
    fn = {"tcp": rtt_tcp, "udp": rtt_udp, "ws": rtt_ws}[a.mode]
    try:
        times, got, dt = fn(a.ip, port, a.count, a.burst)
    except Exception as e:
        print(json.dumps({"mode": a.mode, "ok": 0, "error": str(e)}, ensure_ascii=False))
        return 1
    out = {
        "mode": a.mode,
        "ip": a.ip,
        "port": port,
        "count": a.count,
        "received": got,
        "elapsed_ms": round(dt * 1000, 1),
        "reply_per_s": round(got / dt, 1) if dt > 0 else 0,
        "burst": bool(a.burst),
    }
    if times:
        out.update({
            "rtt_p50_ms": round(statistics.median(times), 3),
            "rtt_p95_ms": round(sorted(times)[max(0, int(len(times) * 0.95) - 1)], 3),
            "rtt_min_ms": round(min(times), 3),
            "rtt_max_ms": round(max(times), 3),
        })
    print(json.dumps(out, ensure_ascii=False))
    return 0 if got == a.count else 2


if __name__ == "__main__":
    raise SystemExit(main())
