#!/usr/bin/env python3
"""Dual-stream raw TCP aggregate test: 5000/5003 sink, 5001/5002 src."""
import argparse, socket, threading, time, json

def run_pair(ip, ports, total, chunk, upload):
    half = total // 2
    results = [0, 0]
    def worker(i):
        port = ports[i]
        try:
            s = socket.create_connection((ip, port), timeout=5)
            s.settimeout(20)
            if upload:
                data = b'X' * min(chunk, 65536)
                left = half
                sent = 0
                while left:
                    n = min(len(data), left)
                    s.sendall(data[:n]); left -= n; sent += n
                results[i] = sent
            else:
                s.sendall(f'GO {half}\n'.encode())
                got = 0; buf = bytearray(65536)
                while got < half:
                    n = s.recv_into(buf, min(len(buf), half - got))
                    if n <= 0: break
                    got += n
                results[i] = got
            s.close()
        except Exception:
            results[i] = 0
    t0 = time.time()
    ts = [threading.Thread(target=worker, args=(i,)) for i in range(2)]
    for t in ts: t.start()
    for t in ts: t.join()
    dt = time.time() - t0
    got = sum(results)
    return got, dt

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ip', required=True)
    ap.add_argument('--total', type=int, default=8*1024*1024)
    ap.add_argument('--chunk', type=int, default=4096)
    a = ap.parse_args()
    out = {'ip': a.ip, 'total': a.total, 'chunk': a.chunk}
    for label, ports, upload in (('upload', (5000, 5003), True), ('download', (5001, 5002), False)):
        try:
            got, dt = run_pair(a.ip, ports, a.total, a.chunk, upload)
            out[label] = {'mbps': round(got/dt/1024/1024, 3), 'got': got, 's': round(dt, 3)}
        except Exception as e:
            out[label] = 'ERR ' + str(e)
    print(json.dumps(out, ensure_ascii=False))

if __name__ == '__main__':
    main()
