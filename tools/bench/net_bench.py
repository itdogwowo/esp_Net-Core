#!/usr/bin/env python3
"""ESP32-P4 純網路基準（裸 TCP，不過 NC4）

下載（PC→ESP, port 5000）：PC 灌資料，裝置計數
上傳（ESP→PC, port 5001/5002）：PC 送 "GO <bytes>"，裝置全力送出

用法:
  python3 tools/bench/net_bench.py --host 10.10.1.50 --dl 16 --ul 16
  python3 tools/bench/net_bench.py --host 10.10.1.50 --dual 16     # 兩條上傳同時跑
"""
import argparse, socket, threading, time

def upload(host, port, mb):
    s = socket.create_connection((host, port), timeout=5)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    total = int(mb * 1024 * 1024)
    s.sendall(b"GO %d\n" % total)
    got = 0
    t0 = time.time()
    while got < total:
        buf = s.recv(65536)
        if not buf:
            break
        got += len(buf)
    dt = time.time() - t0
    s.close()
    return got, dt

def download(host, port, mb):
    s = socket.create_connection((host, port), timeout=5)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    chunk = b"x" * 1460
    total = int(mb * 1024 * 1024)
    sent = 0
    t0 = time.time()
    while sent < total:
        s.sendall(chunk)
        sent += len(chunk)
    s.shutdown(socket.SHUT_WR)
    # 等裝置收完（關閉時 flush）
    try:
        s.recv(1)
    except OSError:
        pass
    dt = time.time() - t0
    s.close()
    return sent, dt

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="10.10.1.50")
    ap.add_argument("--dl", type=float, default=0, help="下載測試 MB")
    ap.add_argument("--ul", type=float, default=0, help="單條上傳 MB")
    ap.add_argument("--dual", type=float, default=0, help="雙條上傳各 MB")
    a = ap.parse_args()

    if a.dl:
        n, dt = download(a.host, 5000, a.dl)
        print(f"下載 PC→ESP: {n/1048576:.2f} MB / {dt:.2f} s = {n/dt/1048576:.2f} MB/s")
    if a.ul:
        n, dt = upload(a.host, 5001, a.ul)
        print(f"上傳 ESP→PC: {n/1048576:.2f} MB / {dt:.2f} s = {n/dt/1048576:.2f} MB/s")
    if a.dual:
        res = {}
        def run(port):
            res[port] = upload(a.host, port, a.dual)
        t0 = time.time()
        th = [threading.Thread(target=run, args=(p,)) for p in (5001, 5002)]
        for t in th: t.start()
        for t in th: t.join()
        dt = time.time() - t0
        total = sum(r[0] for r in res.values())
        for p, (n, d) in sorted(res.items()):
            print(f"  通道 {p}: {n/1048576:.2f} MB / {d:.2f} s = {n/d/1048576:.2f} MB/s")
        print(f"雙通道合計: {total/1048576:.2f} MB / {dt:.2f} s = {total/dt/1048576:.2f} MB/s")

if __name__ == "__main__":
    main()
