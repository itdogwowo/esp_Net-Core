#!/usr/bin/env python3
"""NC4 over UDP 測試（device :6001）

用法:
    tools/bench/nc4_udp_test.py                  # 全部
    tools/bench/nc4_udp_test.py --suite basic    # RTT / 連續指令
    tools/bench/nc4_udp_test.py --suite bench    # 吞吐（1K/4K/8K，batch 1 與多幀）

UDP 沒有連線：回覆會送到「最後一則 datagram 的來源」。
"""
import argparse
import binascii
import socket
import struct
import time

HOST_DEFAULT = "10.10.1.50"
UDP_PORT = 6001

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


class Udp:
    def __init__(self, host, port=UDP_PORT, timeout=2.0):
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.s.settimeout(timeout)
        # macOS 預設對 UDP 設 DF（IP_DONTFRAG=1）→ 超過 MTU 的 datagram 直接 EMSGSIZE。
        # 清掉才測得到「8 KB 一包」這種會切 IP 片段的用法（macOS: IP_DONTFRAG = 28）。
        try:
            self.s.setsockopt(socket.IPPROTO_IP, 28, struct.pack("i", 0))
        except OSError:
            pass
        try:
            self.s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1 << 20)
        except OSError:
            pass
        self.host = host
        self.port = port
        self.addr = (host, port)

    def send(self, data):
        # 一次灌 16 MB 會把 macOS 的 UDP send buffer 打爆（ENOBUFS）；退一步再送
        for _ in range(200):
            try:
                self.s.sendto(data, self.addr)
                return
            except OSError as e:
                if e.errno in (55, 105):
                    time.sleep(0.001)
                    continue
                raise
        raise OSError("UDP send 一直 ENOBUFS")

    def recv(self, timeout=None):
        if timeout is not None:
            self.s.settimeout(timeout)
        try:
            d, _ = self.s.recvfrom(65536)
            return d
        except socket.timeout:
            return None

    def wait_cmd(self, want, timeout=2.0):
        end = time.time() + timeout
        while time.time() < end:
            d = self.recv(max(0.05, end - time.time()))
            if d is None:
                return None
            if len(d) >= 9 and d[:2] == SOF:
                if struct.unpack_from("<H", d, 5)[0] == want:
                    return d
        return None

    def close(self):
        self.s.close()


def status(u):
    t0 = time.time()
    u.send(pack(CMD_STATUS, struct.pack("<H", 3) + b"sys"))
    d = u.wait_cmd(CMD_STATUS_RSP, 2)
    return (time.time() - t0) * 1000, d


def bench(u, size, frames, batch, pace_us=0):
    frame = pack(CMD_BENCH_DATA, b"\xa5" * size)
    msg = frame * batch
    u.send(pack(CMD_BENCH_READY))
    if u.wait_cmd(CMD_BENCH_REPORT, 2) is None:
        return None
    t0 = time.time()
    sent = 0
    while sent < frames:
        k = min(batch, frames - sent)
        u.send(msg if k == batch else frame * k)
        sent += k
        if pace_us:
            time.sleep(pace_us / 1e6)
    # 給裝置一點時間把最後幾則解完（UDP 沒有 ACK）
    time.sleep(0.2)
    u.send(pack(CMD_BENCH_RESULT))
    d = u.wait_cmd(CMD_BENCH_REPORT, 5)
    dt = time.time() - t0
    if d is None or len(d) < 13:
        return None
    n = struct.unpack_from("<I", d, 9)[0]
    got_bytes = n * len(frame)          # 真正解出來的 bytes（不是送出的）
    return n, dt, got_bytes / dt / 1048576


def suite_basic(host):
    ok = True
    u = Udp(host)
    print("1) STATUS（UDP 沒有握手，直接送）")
    dt, d = status(u)
    print(f"   {'OK' if d else 'FAIL'}  第一次回覆 {dt:.1f} ms")
    ok &= bool(d)

    print("2) RTT × 20")
    lat = []
    for _ in range(20):
        dt, d = status(u)
        if d:
            lat.append(dt)
    if lat:
        lat.sort()
        print(f"   OK  中位 {lat[len(lat)//2]:.2f} ms，最小 {lat[0]:.2f}，n={len(lat)}")
    else:
        print("   FAIL 沒有回覆")
        ok = False

    print("3) 連續 100 條（每 2 ms 一條 —— UDP 的建議用法）")
    got = 0
    for _ in range(100):
        u.send(pack(CMD_STATUS, struct.pack("<H", 3) + b"sys"))
        time.sleep(0.002)
        d = u.recv(0.5)
        if d and len(d) >= 9 and struct.unpack_from("<H", d, 5)[0] == CMD_STATUS_RSP:
            got += 1
    print(f"   {'OK' if got >= 95 else 'FAIL'}  {got}/100（有節流）")
    ok &= (got >= 95)

    print("4) 突發 100 條（不節流，只回報不判失敗）")
    for _ in range(100):
        u.send(pack(CMD_STATUS, struct.pack("<H", 3) + b"sys"))
    burst = 0
    end = time.time() + 1.5
    while time.time() < end:
        d = u.recv(0.2)
        if d and len(d) >= 9 and struct.unpack_from("<H", d, 5)[0] == CMD_STATUS_RSP:
            burst += 1
    print(f"   收到 {burst}/100（UDP 沒有流量控制，突發本來就會掉）")
    u.close()
    return ok


def suite_bench(host):
    ok = True
    u = Udp(host, timeout=10)
    print("5) 吞吐 —— (frame大小, 每包幀數,  pacing)")
    cases = [
        (1024, 16000, 1, 0, "1K ×1（1037 B，不切片段）"),
        (1024, 16000, 8, 0, "1K ×8（8296 B，會切片段）"),
        (4096,  4000, 1, 0, "4K ×1（4109 B，會切片段）"),
        (4096,  4000, 1, 500, "4K ×1 + 0.5 ms pacing"),
        (8192,  2000, 1, 0, "8K ×1（8205 B，會切片段）"),
        (8192,  2000, 1, 500, "8K ×1 + 0.5 ms pacing"),
    ]
    for size, frames, batch, pace, label in cases:
        r = bench(u, size, frames, batch, pace)
        if r is None:
            print(f"   FAIL {label}")
            ok = False
            continue
        n, dt, mb = r
        lost = frames - n
        pct = lost * 100.0 / frames
        print(f"   {'OK' if pct <= 2 else 'LOSS'}  {label}: {n}/{frames} 幀"
              f"（掉 {lost}, {pct:.1f}%），{dt:.2f}s，{mb:.2f} MB/s")
        ok &= (pct <= 2.0)
    u.close()
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default=HOST_DEFAULT)
    ap.add_argument("--suite", choices=["all", "basic", "bench"], default="all")
    a = ap.parse_args()

    results = {}
    if a.suite in ("all", "basic"):
        results["basic"] = suite_basic(a.host)
    if a.suite in ("all", "bench"):
        results["bench"] = suite_bench(a.host)

    print("\n=== 結果 ===")
    for k, v in results.items():
        print(f"  {k:6s} {'PASS' if v else 'FAIL'}")
    return 0 if all(results.values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
