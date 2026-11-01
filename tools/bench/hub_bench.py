#!/usr/bin/env python3
"""0xF003 雙核 hub 交換量測（writer core1 / reader core0）

用法:
    tools/bench/hub_bench.py                           # 20000 次，正常模式
    tools/bench/hub_bench.py --iters 100000 --repeat 3
    tools/bench/hub_bench.py --iters 20000 --late      # 重現舊版順序（deadline 太晚）

回應（0xF004）欄位:
    iters    要求的交換次數
    read     reader 實際完成的次數（正常 = iters）
    us       耗時（µs，含開 task 與收尾）
    per_sec  交換率
    torn     canary 撕裂次數（必須 0）
    w        writer 成功 commit 的次數
    w_full   writer 遇到「沒空位」的次數
    r_null   reader 遇到「沒有 READY」的次數
    exit     [writer 結束原因, reader 結束原因] 1=完成 2=逾時 3=等不到 start
    late     1 = 舊版順序（deadline 在開 task 之後才寫）
"""
import argparse
import binascii
import json
import socket
import struct
import time

SOF = b"NC"
ADDR_BROADCAST = 0xFFFF
CMD_HUB_BENCH = 0xF003
CMD_HUB_RSP = 0xF004


def pack(cmd, payload=b"", addr=ADDR_BROADCAST):
    body = struct.pack("<BHHH", 4, addr, cmd, len(payload)) + payload
    return SOF + body + struct.pack("<I", binascii.crc32(body) & 0xFFFFFFFF)


def recv_cmd(s, want, timeout):
    """等一個指定 cmd 的合法幀；回傳 (payload, frame)。"""
    buf = b""
    end = time.time() + timeout
    while time.time() < end:
        i = buf.find(SOF)
        if i >= 0:
            buf = buf[i:]
            if len(buf) >= 13:
                cmd = struct.unpack_from("<H", buf, 5)[0]
                ln = struct.unpack_from("<H", buf, 7)[0]
                if len(buf) >= 9 + ln + 4:
                    body = buf[2:9 + ln]
                    crc = struct.unpack_from("<I", buf, 9 + ln)[0]
                    frame, buf = buf[:9 + ln + 4], buf[9 + ln + 4:]
                    if (binascii.crc32(body) & 0xFFFFFFFF) == crc and cmd == want:
                        data = body[7:7 + ln]
                        # JSON 回覆一律是 str_u16len：前 2 B 是長度
                        n = struct.unpack_from("<H", data, 0)[0] if len(data) >= 2 else 0
                        return data[2:2 + n], frame
                    continue
        s.settimeout(max(0.05, end - time.time()))
        try:
            d = s.recv(65536)
        except socket.timeout:
            break
        if not d:
            break
        buf += d
    return None, None


def run_once(host, port, iters, late, timeout):
    s = socket.create_connection((host, port), timeout=5)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    time.sleep(0.15)
    payload = struct.pack("<I", iters) + (b"\x01" if late else b"\x00")
    t0 = time.time()
    s.sendall(pack(CMD_HUB_BENCH, payload))
    body, _ = recv_cmd(s, CMD_HUB_RSP, timeout)
    dt = time.time() - t0
    s.close()
    if body is None:
        return None, dt
    return json.loads(body), dt


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="10.10.1.50")
    ap.add_argument("--port", type=int, default=6000)
    ap.add_argument("--iters", type=int, default=20000)
    ap.add_argument("--repeat", type=int, default=1)
    ap.add_argument("--late", action="store_true", help="重現舊版（deadline 太晚）")
    ap.add_argument("--timeout", type=float, default=10)
    a = ap.parse_args()

    for i in range(a.repeat):
        r, dt = run_once(a.host, a.port, a.iters, a.late, a.timeout)
        if r is None:
            print(f"[{i}] 沒有回應（{dt:.2f} s）")
            continue
        ok = "OK" if (r["read"] == r["iters"] and r["torn"] == 0) else "FAIL"
        per = r["us"] / r["read"] if r["read"] else 0.0
        print(f"[{i}] {ok}  iters={r['iters']} read={r['read']} w={r['w']} "
              f"{r['us']/1000:.1f} ms  {r['per_sec']:.0f}/s  "
              f"{per:.2f} us/次  "
              f"torn={r['torn']} w_full={r['w_full']} r_null={r['r_null']} "
              f"exit={r['exit']} late={r.get('late', 0)}  (wall {dt:.2f} s)")


if __name__ == "__main__":
    main()
