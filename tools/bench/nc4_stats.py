#!/usr/bin/env python3
"""0xF008 匯流排統計 + 零複製驗證

用法:
    tools/bench/nc4_stats.py --reset          # 歸零計數
    tools/bench/nc4_stats.py                  # 讀統計
    tools/bench/nc4_stats.py --run 16         # 歸零 → 灌 16 MB → 讀統計
    tools/bench/nc4_stats.py --edge           # 碎片 + 垃圾位元組的重同步測試

看什麼:
    copied        為了把 payload 交給 parser 而做的 memcpy bytes
                  frame 模式（零複製）應該是 0；stream 模式約等於 2 × 位元組數
    us_per_frame  解析一幀平均花多少 µs（frame 模式不含任何複製）
    resync        丟掉幾個 byte 才重新同步（正常 0；垃圾測試會 > 0）
"""
import argparse
import binascii
import json
import os
import socket
import struct
import sys
import time

SOF = b"NC"
CMD_BUS_STATS = 0xF008
CMD_BUS_RSP = 0xF009
CMD_BENCH_READY = 0x1811
CMD_BENCH_DATA = 0x1812
CMD_BENCH_RESULT = 0x1813
CMD_BENCH_REPORT = 0x1814
CMD_STATUS = 0x1101


def pack(cmd, payload=b"", addr=0xFFFF):
    body = struct.pack("<BHHH", 4, addr, cmd, len(payload)) + payload
    return SOF + body + struct.pack("<I", binascii.crc32(body) & 0xFFFFFFFF)


class Conn:
    def __init__(self, host, port, timeout=20):
        self.s = socket.create_connection((host, port), timeout=5)
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buf = b""
        self.timeout = timeout
        time.sleep(0.2)

    def send(self, data):
        self.s.sendall(data)

    def wait_cmd(self, want, timeout=None):
        timeout = timeout or self.timeout
        end = time.time() + timeout
        while time.time() < end:
            i = self.buf.find(SOF)
            if i >= 0:
                self.buf = self.buf[i:]
                while len(self.buf) >= 13:
                    cmd = struct.unpack_from("<H", self.buf, 5)[0]
                    ln = struct.unpack_from("<H", self.buf, 7)[0]
                    if len(self.buf) < 9 + ln + 4:
                        break
                    body = self.buf[2:9 + ln]
                    crc = struct.unpack_from("<I", self.buf, 9 + ln)[0]
                    frame, self.buf = self.buf[:9 + ln + 4], self.buf[9 + ln + 4:]
                    if (binascii.crc32(body) & 0xFFFFFFFF) != crc:
                        continue
                    if cmd == want:
                        return frame[9:9 + ln]      # 原始 payload（JSON 或二進位都可能）
                    if cmd == CMD_BENCH_REPORT:
                        continue
            self.s.settimeout(max(0.05, end - time.time()))
            try:
                d = self.s.recv(65536)
            except socket.timeout:
                break
            if not d:
                break
            self.buf += d
        return None

    def close(self):
        self.s.close()


def json_of(data):
    """str_u16len JSON → dict；其他格式回 None。"""
    if not data or len(data) < 2:
        return None
    n = struct.unpack_from("<H", data, 0)[0]
    try:
        return json.loads(data[2:2 + n])
    except Exception:
        return None


def stats(c, reset=False):
    c.send(pack(CMD_BUS_STATS, b"\x01" if reset else b"\x00"))
    return json_of(c.wait_cmd(CMD_BUS_RSP, 5))


def show(label, s):
    print(f"{label}: resync={s['resync']}")
    for b in s["bus"]:
        print(f"  {b['name']:5s} {b['mode']:11s} frame={b['frame']} "
              f"frames={b['frames']:6d} {b['mb']:7.2f} MB  "
              f"copied={b['copied']:12d} B  parse={b['parse_ms']:8.1f} ms  "
              f"{b['us_per_frame']:7.1f} us/幀")


def run_bulk(c, mb):
    frame = pack(CMD_BENCH_DATA, b"\xa5" * 8192)
    total = int(mb * 1048576)
    sent = 0
    c.s.settimeout(60)
    t0 = time.time()
    c.send(pack(CMD_BENCH_READY))
    c.wait_cmd(CMD_BENCH_REPORT, 5)
    while sent < total:
        k = max(1, min(500, (total - sent) // len(frame)))
        if k == 0:
            break
        c.send(frame * k)
        sent += k * len(frame)
    c.s.settimeout(20)
    c.send(pack(CMD_BENCH_RESULT))
    d = c.wait_cmd(CMD_BENCH_REPORT, 20)
    got = struct.unpack_from("<I", d, 0)[0] if d else -1
    dt = time.time() - t0
    print(f"灌 {sent/1048576:.1f} MB → 裝置計數 {got} 幀，{dt:.2f} s，"
          f"{sent/dt/1048576:.2f} MB/s")


def edge(c):
    """碎片 + 垃圾：都必須恢復"""
    ok = True
    # 1) 一幀拆成 3 段送（每段間隔 20 ms）
    payload = struct.pack("<H", 3) + b"sys"
    fr = pack(CMD_STATUS, payload)
    for part in (fr[:5], fr[5:12], fr[12:]):
        c.send(part)
        time.sleep(0.02)
    d = c.wait_cmd(0x1102, 5)
    print(f"1) 碎片幀（3 段）: {'OK' if d else 'FAIL'}")
    ok &= bool(d)

    # 2) 先塞 37 bytes 垃圾，再送合法幀
    c.send(bytes((i * 7) & 0xFF for i in range(37)))
    c.send(pack(CMD_STATUS, payload))
    d = c.wait_cmd(0x1102, 5)
    print(f"2) 37 B 垃圾後重同步: {'OK' if d else 'FAIL'}")
    ok &= bool(d)

    # 3) 垃圾中間剛好含 "NC" + 合法 VER，但長度唬人 → 不能卡住
    c.send(SOF + bytes([4]) + b"\xff\xff" + struct.pack("<H", 0x1101) + b"\xff\xff" + b"x" * 10)
    time.sleep(0.1)
    c.send(pack(CMD_STATUS, payload))
    d = c.wait_cmd(0x1102, 5)
    print(f"3) 假表頭（plen=65535）: {'OK' if d else 'FAIL'}")
    ok &= bool(d)

    # 4) 大幀：8 KB payload
    c.send(pack(CMD_BENCH_DATA, b"\x5a" * 8192))
    c.send(pack(CMD_STATUS, payload))
    d = c.wait_cmd(0x1102, 5)
    print(f"4) 8 KB 幀之後的指令: {'OK' if d else 'FAIL'}")
    ok &= bool(d)
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="10.10.1.50")
    ap.add_argument("--port", type=int, default=6000)
    ap.add_argument("--reset", action="store_true")
    ap.add_argument("--run", type=float, default=0, help="灌多少 MB 再讀統計")
    ap.add_argument("--edge", action="store_true")
    a = ap.parse_args()

    c = Conn(a.host, a.port)
    if a.reset:
        show("歸零後", stats(c, reset=True))
    if a.run:
        stats(c, reset=True)
        run_bulk(c, a.run)
        show("灌完", stats(c))
    if a.edge:
        ok = edge(c)
        s = stats(c)
        show("邊界測試後", s)
        print("總結果:", "全部通過" if ok else "有失敗")
    if not (a.reset or a.run or a.edge):
        show("統計", stats(c))
    c.close()


if __name__ == "__main__":
    main()
