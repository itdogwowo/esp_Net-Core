#!/usr/bin/env python3
"""NC4 over WebSocket 全套測試

用法:
    tools/bench/nc4_ws_test.py                 # 全部跑
    tools/bench/nc4_ws_test.py --suite basic   # 連線 / RTT / 連續指令
    tools/bench/nc4_ws_test.py --suite bench   # 吞吐（1K / 4K / 8K）
    tools/bench/nc4_ws_test.py --suite edge    # 碎片 / 垃圾 / 過大 / 重連 / 並存

需要: pip install websocket-client
"""
import argparse
import binascii
import socket
import struct
import time

from websocket import ABNF, create_connection

HOST_DEFAULT = "10.10.1.50"
WS_PORT = 8080
TCP_PORT = 6000

CMD_STATUS = 0x1101
CMD_STATUS_RSP = 0x1102
CMD_BENCH_READY = 0x1811
CMD_BENCH_DATA = 0x1812
CMD_BENCH_RESULT = 0x1813
CMD_BENCH_REPORT = 0x1814

SOF = b"NC"


def pack(cmd, payload=b"", addr=0xFFFF):
    body = struct.pack("<BHHH", 4, addr, cmd, len(payload)) + payload
    return SOF + body + struct.pack("<I", binascii.crc32(body) & 0xFFFFFFFF)


class Ws:
    def __init__(self, host, port=WS_PORT, timeout=10):
        self.ws = create_connection(f"ws://{host}:{port}/nc4", timeout=timeout)
        self.ws.settimeout(timeout)

    def send(self, data):
        self.ws.send_binary(data)

    def send_frames(self, parts, opcode=ABNF.OPCODE_BINARY):
        """手動碎片：parts 會變成多個 WS frame，最後一個 fin=1"""
        n = len(parts)
        for i, p in enumerate(parts):
            fin = (i == n - 1)
            op = opcode if i == 0 else ABNF.OPCODE_CONT
            self.ws.send_frame(ABNF.create_frame(p, op, fin=fin))

    def recv_msg(self, timeout=5):
        """收一則訊息（bytes）；超時/斷線回 None"""
        self.ws.settimeout(max(0.05, timeout))
        try:
            d = self.ws.recv()
        except Exception:
            return None
        if isinstance(d, str):
            d = d.encode()
        return d

    def wait_cmd(self, want, timeout=10):
        end = time.time() + timeout
        while time.time() < end:
            d = self.recv_msg(max(0.05, end - time.time()))
            if d is None:
                return None
            if len(d) >= 9 and d[:2] == SOF:
                cmd = struct.unpack_from("<H", d, 5)[0]
                if cmd == want:
                    return d
        return None

    def close(self):
        try:
            self.ws.close()
        except Exception:
            pass


def status_ws(ws):
    t0 = time.time()
    ws.send(pack(CMD_STATUS, struct.pack("<H", 3) + b"sys"))
    d = ws.wait_cmd(CMD_STATUS_RSP, 3)
    return (time.time() - t0) * 1000, d


# ───────────────────────── suites ─────────────────────────

def suite_basic(host):
    ok = True
    ws = Ws(host)
    print("1) 連線 + STATUS")
    dt, d = status_ws(ws)
    print(f"   {'OK' if d else 'FAIL'}  第一次回覆 {dt:.1f} ms")
    ok &= bool(d)

    print("2) RTT × 20")
    lat = []
    for _ in range(20):
        dt, d = status_ws(ws)
        if d:
            lat.append(dt)
    if lat:
        lat.sort()
        print(f"   OK  中位 {lat[len(lat)//2]:.2f} ms，最小 {lat[0]:.2f}")
    else:
        print("   FAIL 沒有回覆")
        ok = False

    print("3) 連續 100 條指令")
    t0 = time.time()
    got = 0
    for _ in range(100):
        _, d = status_ws(ws)
        got += bool(d)
    dt = time.time() - t0
    print(f"   {'OK' if got == 100 else 'FAIL'}  {got}/100，{dt:.2f}s，{100/dt:.0f} 條/s")
    ok &= (got == 100)

    ws.close()
    return ok


def bench(ws, size, frames, batch=1):
    """batch = 一則 WS 訊息塞幾個 NC4 幀（攤提 httpd 的每則固定成本）"""
    frame = pack(CMD_BENCH_DATA, b"\xa5" * size)
    ws.send(pack(CMD_BENCH_READY))
    if ws.wait_cmd(CMD_BENCH_REPORT, 5) is None:
        return None
    msg = frame * batch
    t0 = time.time()
    sent = 0
    while sent < frames:
        k = min(batch, frames - sent)
        if k == batch:
            ws.send(msg)
        else:
            ws.send(frame * k)
        sent += k
    ws.send(pack(CMD_BENCH_RESULT))
    d = ws.wait_cmd(CMD_BENCH_REPORT, 60)
    dt = time.time() - t0
    if d is None or len(d) < 13:
        return None
    n = struct.unpack_from("<I", d, 9)[0]
    total = frames * len(frame)
    return n, dt, total / dt / 1048576


def suite_bench(host):
    ok = True
    ws = Ws(host, timeout=120)
    print("4) 吞吐（約 16 MB）—— batch = 一則 WS 訊息幾個 NC4 幀")
    for size, frames in ((1024, 16000), (4096, 4000), (8192, 2000)):
        max_per_msg = 24000 // (size + 13)      # 裝置端 WS_MAX_MSG = 24 KB
        for batch in (1, max(1, min(8, max_per_msg))):
            r = bench(ws, size, frames, batch)
            if r is None:
                print(f"   FAIL {size // 1024}K batch={batch}")
                ok = False
                continue
            n, dt, mb = r
            print(f"   {'OK' if n == frames else 'FAIL'}  {size//1024:>2d}K × {frames} "
                  f"(batch={batch}): {n}/{frames} 幀，{dt:.2f}s，{mb:.2f} MB/s")
            ok &= (n == frames)
    ws.close()
    return ok


def suite_edge(host):
    ok = True
    print("5) 一則訊息拆成 3 個 WS frame")
    ws = Ws(host)
    fr = pack(CMD_STATUS, struct.pack("<H", 3) + b"sys")
    ws.send_frames([fr[:5], fr[5:12], fr[12:]])
    d = ws.wait_cmd(CMD_STATUS_RSP, 5)
    print(f"   {'OK' if d else 'FAIL'}")
    ok &= bool(d)

    print("6) 垃圾位元組（不是 NC4 幀）→ 連線要活著")
    ws.send(b"\x00" * 37)
    _, d = status_ws(ws)
    print(f"   {'OK' if d else 'FAIL'}")
    ok &= bool(d)

    print("7) 過大訊息（25000 B > 24576）→ 應該被拒")
    try:
        ws.send(b"\x5a" * 25000)
        d = ws.recv_msg(2)
        rejected = d is None or len(d) == 0
    except Exception:
        rejected = True
    print(f"   {'OK' if rejected else 'FAIL'}（{'已斷線' if rejected else '竟然收了'}）")
    ok &= rejected
    ws.close()

    print("8) 重連")
    ws = Ws(host)
    _, d = status_ws(ws)
    print(f"   {'OK' if d else 'FAIL'}")
    ok &= bool(d)

    print("9) 第二條 WS 應該被拒")
    try:
        ws2 = Ws(host, timeout=3)
        _, d2 = status_ws(ws2)
        rejected2 = d2 is None
        ws2.close()
    except Exception:
        rejected2 = True
    print(f"   {'OK' if rejected2 else 'FAIL'}")
    ok &= rejected2

    print("10) WS 與 TCP 並存")
    try:
        t = socket.create_connection((host, TCP_PORT), timeout=5)
        t.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        t.sendall(pack(CMD_STATUS, struct.pack("<H", 3) + b"sys"))
        t.settimeout(3)
        td = t.recv(512)
        _, wd = status_ws(ws)
        t.close()
        both = bool(td) and bool(wd)
    except Exception as e:
        print(f"   FAIL {e}")
        both = False
    print(f"   {'OK' if both else 'FAIL'}")
    ok &= both
    ws.close()
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default=HOST_DEFAULT)
    ap.add_argument("--suite", choices=["all", "basic", "bench", "edge"], default="all")
    a = ap.parse_args()

    results = {}
    if a.suite in ("all", "basic"):
        results["basic"] = suite_basic(a.host)
    if a.suite in ("all", "bench"):
        results["bench"] = suite_bench(a.host)
    if a.suite in ("all", "edge"):
        results["edge"] = suite_edge(a.host)

    print("\n=== 結果 ===")
    for k, v in results.items():
        print(f"  {k:6s} {'PASS' if v else 'FAIL'}")
    return 0 if all(results.values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
