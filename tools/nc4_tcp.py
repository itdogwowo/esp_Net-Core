#!/usr/bin/env python3
"""NC4 over TCP 測試（P4 乙太網, port 6000）"""
import argparse, socket, struct, binascii, time

SOF = b"NC"
def pack(cmd, payload=b"", addr=0xFFFF):
    body = struct.pack("<BHHH", 4, addr, cmd, len(payload)) + payload
    return SOF + body + struct.pack("<I", binascii.crc32(body) & 0xFFFFFFFF)

class Reader:
    def __init__(self, sock): self.s = sock; self.buf = b""
    def wait_for(self, cmds, timeout=5.0):
        t0 = time.time()
        while time.time() - t0 < timeout:
            while len(self.buf) >= 13:
                i = self.buf.find(SOF)
                if i < 0: self.buf = b""; break
                self.buf = self.buf[i:]
                if len(self.buf) < 13: break
                cmd = struct.unpack_from("<H", self.buf, 5)[0]
                ln = struct.unpack_from("<H", self.buf, 7)[0]
                if len(self.buf) < 9 + ln + 4: break
                body = self.buf[2:9 + ln]
                crc = struct.unpack_from("<I", self.buf, 9 + ln)[0]
                frame = self.buf[:9 + ln + 4]
                self.buf = self.buf[9 + ln + 4:]
                if (binascii.crc32(body) & 0xFFFFFFFF) == crc and cmd in cmds:
                    return frame
            self.s.settimeout(max(0.05, timeout - (time.time() - t0)))
            try:
                d = self.s.recv(65536)
            except socket.timeout:
                break
            if not d: break
            self.buf += d
        return None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="10.10.1.50")
    ap.add_argument("--port", type=int, default=6000)
    ap.add_argument("--rtt", type=int, default=20)
    ap.add_argument("--frames", type=int, default=2000)
    ap.add_argument("--size", type=int, default=8192)
    a = ap.parse_args()

    s = socket.create_connection((a.host, a.port), timeout=5)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    rd = Reader(s)
    time.sleep(0.2)

    # 1) 控制幀 RTT
    lat = []
    for _ in range(a.rtt):
        t0 = time.time()
        s.sendall(pack(0x1101, struct.pack("<H", 3) + b"sys"))
        f = rd.wait_for({0x1102}, timeout=2)
        if f: lat.append((time.time() - t0) * 1000)
    if lat:
        lat.sort()
        print(f"STATUS RTT: 中位 {lat[len(lat)//2]:.2f} ms, 最小 {lat[0]:.2f}, n={len(lat)}")

    # 2) 純解碼吞吐：灌 BENCH_DATA，再取 RESULT
    s.sendall(pack(0x1811)); rd.wait_for({0x1813}, timeout=2)
    payload = b"\xa5" * a.size
    blob = pack(0x1812, payload) * a.frames
    t0 = time.time()
    # 16 MB 在 ~5 MB/s 下要 3 秒以上：不能沿用 Reader 每次收斂後的 2 秒 timeout，
    # 否則 sendall 會在裝置還在收的時候就逾時（之前的「卡住」有一半是這個）
    s.settimeout(60)
    s.sendall(blob)
    s.settimeout(20)
    s.sendall(pack(0x1813))
    f = rd.wait_for({0x1814}, timeout=20)
    dt = time.time() - t0
    if f:
        ln = struct.unpack_from("<H", f, 7)[0]
        val = struct.unpack_from("<I", f, 9)[0]
        total = a.frames * (9 + a.size + 4)
        print(f"NC4 解碼: 送 {a.frames} 幀 × {a.size} B → 收到 {val} 幀，"
              f"{dt:.2f} s，{total/dt/1048576:.2f} MB/s，{a.frames/dt:.0f} 幀/s")
    s.close()

if __name__ == "__main__":
    main()
