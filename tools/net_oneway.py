#!/usr/bin/env python3
"""NC4 一發多收（0xF040 NETBENCH）真實測速。

例子：
  python3 tools/net_oneway.py --ip 10.10.1.31 --mode tcp --count 100 \
      --payload 512 --batch 100 --serial /dev/cu.usbmodemXXXX
  python3 tools/net_oneway.py --ip 10.10.1.31 --mode udp --count 100 \
      --payload 512 --batch 2 --pacing 0.005 --serial /dev/...
  python3 tools/net_oneway.py --ip 10.10.1.31 --mode ws --count 100 \
      --payload 512 --batch 10 --serial /dev/...

- 唔用 --serial 時，result 走同一條網絡線（UDP 可能掉，會 retry）。
- result 內的 frames／lost／missing 是 **device 內部統計**，不是主機自己數。
"""
from __future__ import annotations

import argparse
import json
import socket
import struct
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from nc4_demux import pack, Reader, ADDR_BROADCAST  # noqa: E402

CMD = 0xF040


def start_frame():
    return pack(CMD, b"\x00", ADDR_BROADCAST)


def data_frame(seq: int, payload: bytes):
    return pack(CMD, b"\x03" + struct.pack("<I", seq) + payload, ADDR_BROADCAST)


def result_frame():
    return pack(CMD, b"\x02", ADDR_BROADCAST)


def frames(buf: bytes):
    out = []
    while len(buf) >= 9:
        n = struct.unpack_from("<H", buf, 7)[0]
        total = 9 + n + 4
        if len(buf) < total:
            break
        out.append(buf[:total])
        buf = buf[total:]
    return out, buf


def body(fr: bytes) -> bytes:
    return fr[9:-4]


def serial_result(port: str) -> dict:
    import serial
    s = serial.Serial(port, 115200, timeout=0.2)
    try:
        s.reset_input_buffer()
        s.write(result_frame())
        r = Reader(s).wait_for({0xF004}, 6)
        n = struct.unpack_from("<H", r[1])[0]
        return json.loads(r[1][2:2 + n].decode())
    finally:
        s.close()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip", required=True)
    ap.add_argument("--mode", choices=("tcp", "udp", "ws"), required=True)
    ap.add_argument("--port", type=int, default=0)
    ap.add_argument("--count", type=int, default=100)
    ap.add_argument("--payload", type=int, default=512)
    ap.add_argument("--batch", type=int, default=1)
    ap.add_argument("--pacing", type=float, default=0.0)
    ap.add_argument("--serial", default="")
    ap.add_argument("--timeout", type=float, default=5.0,
                    help="等 device 內部 frames 到齊的最長秒數")
    a = ap.parse_args()
    port = a.port or {"tcp": 6000, "udp": 6001, "ws": 8080}[a.mode]
    payload = b"X" * a.payload
    frames_to_send = [data_frame(i, payload) for i in range(a.count)]

    if a.mode == "tcp":
        s = socket.create_connection((a.ip, port), timeout=3)
        s.settimeout(3)
        send = s.sendall
    elif a.mode == "udp":
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.settimeout(3)
        send = lambda b: s.sendto(b, (a.ip, port))  # noqa: E731
    else:
        import websocket
        s = websocket.create_connection(f"ws://{a.ip}:{port}/nc4", timeout=3)
        send = s.send_binary

    # 讓上一輪殘留的 bus/queue 先排空；否則 start 清 counters 後，舊訊息會再被計一次。
    time.sleep(0.5)
    # start（讀掉回覆，避免 TCP unread RST）
    send(start_frame())
    if a.mode in ("tcp", "udp", "ws"):
        try:
            if a.mode == "tcp":
                s.recv(4096)
            elif a.mode == "udp":
                s.recvfrom(4096)
            else:
                s.recv()
        except Exception:
            pass

    t_start = time.time()
    for i in range(0, len(frames_to_send), max(1, a.batch)):
        chunk = frames_to_send[i:i + max(1, a.batch)]
        if a.mode == "udp":
            # 一個 datagram 唔好超過 ~1200 B payload；過大就逐幀送。
            blob = b"".join(chunk)
            if len(blob) > 1200:
                for fr in chunk:
                    send(fr)
            else:
                send(blob)
        else:
            send(b"".join(chunk))
        if a.pacing > 0:
            time.sleep(a.pacing)
    send_s = time.time() - t_start
    # 裝置是 async producer：送完不代表 handler 已做完。等內部 frames 到齊，
    # 而不是固定 0.5s 就查（大幀會誤判成掉幀）。
    res = None
    if a.serial:
        deadline = time.time() + a.timeout
        while time.time() < deadline:
            res = serial_result(a.serial)
            if res.get("frames", 0) >= a.count:
                break
            if res.get("lost", 0) > 0 and (res.get("frames", 0) + res.get("lost", 0)) >= a.count:
                break
            time.sleep(0.2)
    else:
        res = None
        s.settimeout(2)
        for _ in range(5):
            send(result_frame())
            try:
                if a.mode == "tcp":
                    d = s.recv(65536)
                elif a.mode == "udp":
                    d, _ = s.recvfrom(65536)
                else:
                    d = s.recv()
                    if isinstance(d, str):
                        d = d.encode()
            except Exception:
                continue
            fs, _ = frames(d)
            for fr in fs:
                try:
                    j = json.loads(body(fr).decode())
                    if "frames" in j:
                        res = j
                        break
                except Exception:
                    pass
            if res:
                break
    s.close()

    wall_s = time.time() - t_start
    out = {
        "mode": a.mode,
        "count": a.count,
        "payload": a.payload,
        "batch": a.batch,
        "pacing_s": a.pacing,
        "send_s": round(send_s, 4),
        "wall_s": round(wall_s, 4),
        "result": res,
    }
    if res:
        ok = (res.get("frames") == a.count and res.get("lost", 0) == 0)
        # 內部 timestamp 也有污染可能（舊 queue／同一 tick）。S3 WiFi 的合理上限
        # 保守設 20 MB/s；超過就標 invalid，不拿去報速度。
        us = res.get("us") or 0
        mbps = (res.get("bytes", 0) * 1000000.0 / us / 1024.0 / 1024.0) if us > 0 else 0.0
        valid = ok and us >= 100 and mbps <= 20.0
        out["ok"] = valid
        out["speed_mbps"] = round(mbps, 3)
        if not valid:
            out["invalid_reason"] = ("not_all_received" if not ok else
                                     "timer_too_small_or_impossible")
        if wall_s > 0 and res.get("bytes"):
            out["wall_fps"] = round(res["frames"] / wall_s, 1)
            out["wall_kbps"] = round(res["bytes"] / wall_s / 1024.0, 2)
    print(json.dumps(out, ensure_ascii=False))
    return 0 if out.get("ok") else 2


if __name__ == "__main__":
    raise SystemExit(main())
