#!/usr/bin/env python3
"""0xF007 NC4 上傳（打包 + 送出）量測 —— device → PC

用法:
    tools/bench/nc4_tx_bench.py                       # 2000 × 8 KB，同核（預設）
    tools/bench/nc4_tx_bench.py --mode split          # 分核：core1 打包、core0 送出
    tools/bench/nc4_tx_bench.py --frames 2000 --size 8192 --repeat 3

每跑一次，裝置會「打包 N 幀並全部送出」，PC 端收完 N × (13+size) bytes 後
才會收到 0xF004 的統計。兩邊的速率都會印出來：

    dev   = 裝置自己量的 pack+send 時間
    pc    = PC 從送出命令到收滿所有 bytes 的牆鐘時間
"""
import argparse
import binascii
import json
import socket
import struct
import time

SOF = b"NC"
CMD_TX_BENCH = 0xF007
CMD_RSP = 0xF004


def pack(cmd, payload=b"", addr=0xFFFF):
    body = struct.pack("<BHHH", 4, addr, cmd, len(payload)) + payload
    return SOF + body + struct.pack("<I", binascii.crc32(body) & 0xFFFFFFFF)


def run_once(host, port, frames, size, mode, timeout):
    total = 13 + size            # 每幀 bytes（9 header + payload + 4 CRC）
    want = frames * total
    s = socket.create_connection((host, port), timeout=5)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    time.sleep(0.15)

    payload = struct.pack("<IHB", frames, size, mode)
    t0 = time.time()
    s.sendall(pack(CMD_TX_BENCH, payload))
    s.settimeout(timeout)

    buf = b""
    got = 0                       # 收到的 BENCH_DATA bytes
    t_last = None
    rsp = None
    end = time.time() + timeout
    while time.time() < end:
        d = s.recv(262144)
        if not d:
            break
        buf += d
        # 解析：數 BENCH_DATA，找 0xF004
        while True:
            i = buf.find(SOF)
            if i < 0:
                buf = b""
                break
            buf = buf[i:]
            if len(buf) < 13:
                break
            cmd = struct.unpack_from("<H", buf, 5)[0]
            ln = struct.unpack_from("<H", buf, 7)[0]
            if len(buf) < 9 + ln + 4:
                break
            body = buf[2:9 + ln]
            crc = struct.unpack_from("<I", buf, 9 + ln)[0]
            frame, buf = buf[:9 + ln + 4], buf[9 + ln + 4:]
            if (binascii.crc32(body) & 0xFFFFFFFF) != crc:
                continue
            if cmd == CMD_RSP:
                data = frame[9:9 + ln]
                n = struct.unpack_from("<H", data, 0)[0]
                rsp = json.loads(data[2:2 + n])
                t_last = time.time()
                break
            got += len(frame)
        if rsp is not None:
            break
    s.close()
    wall = (t_last or time.time()) - t0
    return rsp, got, want, wall


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="10.10.1.50")
    ap.add_argument("--port", type=int, default=6000)
    ap.add_argument("--frames", type=int, default=2000)
    ap.add_argument("--size", type=int, default=8192)
    ap.add_argument("--mode", choices=["same", "split"], default="same")
    ap.add_argument("--repeat", type=int, default=1)
    ap.add_argument("--timeout", type=float, default=90)
    a = ap.parse_args()

    mode = 0 if a.mode == "same" else 1
    for i in range(a.repeat):
        rsp, got, want, wall = run_once(a.host, a.port, a.frames, a.size,
                                        mode, a.timeout)
        if rsp is None:
            print(f"[{i}] 沒有回應（收到 {got}/{want} B）")
            continue
        ok = "OK" if (got == want and rsp["sent"] == a.frames) else "FAIL"
        print(f"[{i}] {ok}  {a.mode}  {rsp['sent']}/{rsp['frames']} 幀 × {a.size} B  "
              f"PC {got/1048576:.1f}/{want/1048576:.1f} MB  "
              f"dev {rsp['us']/1000:.0f} ms = {rsp['mb_s']:.2f} MB/s  "
              f"pc {wall:.2f} s = {got/wall/1048576:.2f} MB/s")


if __name__ == "__main__":
    main()
