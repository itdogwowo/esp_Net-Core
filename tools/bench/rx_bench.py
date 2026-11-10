#!/usr/bin/env python3
"""接收路徑跑分：RTT 分佈、吞吐、匯流排的 copy／解析帳。

改動 parser／緩衝形狀前後，用**同一個方法**量同一個東西。

兩個模式：
    rtt   一次送一幀、等回覆 → 每幀 RTT（最敏感，唔會被回覆擠壓影響）
    rate  一次過打 N 幀（可 burst）→ 回覆率與幀數／秒

板側數字來自 `STATUS keys=net` → `net.bus[]`：frames／copied／parse_ms／slot_full。

用法：
    tools/bench/rx_bench.py --port COM8 --mode rtt  --n 200
    tools/bench/rx_bench.py --port COM8 --mode rate --n 500 --burst 16
"""
import argparse
import json
import queue
import statistics
import struct
import sys
import threading
import time
import os

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from nc4_demux import ADDR_BROADCAST, Demux, pack  # noqa: E402

CMD_STATUS_GET = 0x1101
CMD_STATUS_RSP = 0x1102


class Pump:
    """背景讀者：把 RX 的文字與幀分開，只把指定 cmd 的幀放入 queue。"""

    def __init__(self, ser, want: int):
        self.ser = ser
        self.want = want
        self.q: "queue.Queue[bytes]" = queue.Queue()
        self.dm = Demux()
        self.stop = False
        self.t = threading.Thread(target=self._run, daemon=True)
        self.t.start()

    def _run(self):
        while not self.stop:
            try:
                data = self.ser.read(4096)
            except Exception:
                break
            if not data:
                continue
            for kind, v in self.dm.feed(data):
                if kind == "frame":
                    cmd, payload = v
                    if cmd == self.want:
                        self.q.put(payload)

    def wait(self, timeout: float):
        try:
            return self.q.get(timeout=timeout)
        except queue.Empty:
            return None

    def close(self):
        self.stop = True


def status(ser, pump_holder, keys: str):
    """送 STATUS_GET（用一個獨立的 pump，避免同量測搶同一個 queue）。"""
    payload = struct.pack("<H", len(keys)) + keys.encode()
    p = Pump(ser, CMD_STATUS_RSP)
    try:
        ser.write(pack(CMD_STATUS_GET, payload, ADDR_BROADCAST))
        body = p.wait(3.0)
    finally:
        p.close()
    if body is None:
        return None
    n = struct.unpack_from("<H", body)[0]
    try:
        return json.loads(body[2:2 + n].decode("utf-8", "replace"))
    except json.JSONDecodeError:
        return None


def buses(st):
    if not st:
        return {}
    return {b.get("name", "?"): b for b in (st.get("net", {}).get("bus") or [])}


def pct(v, p):
    if not v:
        return 0.0
    v = sorted(v)
    return v[min(len(v) - 1, int(round((p / 100.0) * (len(v) - 1))))]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--mode", choices=("rtt", "rate"), default="rtt")
    ap.add_argument("--n", type=int, default=200)
    ap.add_argument("--keys", default="sys")
    ap.add_argument("--burst", type=int, default=16)
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    import serial

    payload = struct.pack("<H", len(args.keys)) + args.keys.encode()

    with serial.Serial(args.port, args.baud, timeout=0.05) as s:
        time.sleep(0.6)
        before = buses(status(s, None, "net"))
        if not before:
            print("拿不到 net.bus", file=sys.stderr)
            return 1

        rtts = []
        got = 0
        t0 = time.perf_counter()

        if args.mode == "rtt":
            p = Pump(s, CMD_STATUS_RSP)
            try:
                for _ in range(args.n):
                    a = time.perf_counter()
                    s.write(pack(CMD_STATUS_GET, payload, ADDR_BROADCAST))
                    if p.wait(1.0) is not None:
                        got += 1
                        rtts.append((time.perf_counter() - a) * 1000.0)
            finally:
                p.close()
        else:
            p = Pump(s, CMD_STATUS_RSP)
            try:
                sent = 0
                while sent < args.n:
                    batch = min(args.burst, args.n - sent)
                    for _ in range(batch):
                        s.write(pack(CMD_STATUS_GET, payload, ADDR_BROADCAST))
                    sent += batch
                    # 收得到幾多算幾多（rate 模式唔等齊）
                    deadline = time.perf_counter() + 0.25
                    while sent - got > 0 and time.perf_counter() < deadline:
                        if p.wait(0.02) is not None:
                            got += 1
            finally:
                p.close()

        elapsed = time.perf_counter() - t0
        # 等匯流排把最後幾幀處理完，統計才穩定
        time.sleep(0.3)
        after = buses(status(s, None, "net"))

    res = {
        "mode": args.mode,
        "sent": args.n,
        "replied": got,
        "elapsed_s": round(elapsed, 3),
        "frames_per_s": round(got / elapsed, 1) if elapsed > 0 else 0,
        "rtt_ms": {
            "p50": round(pct(rtts, 50), 3),
            "p99": round(pct(rtts, 99), 3),
            "max": round(max(rtts), 3) if rtts else 0,
            "mean": round(statistics.fmean(rtts), 3) if rtts else 0,
        },
        "bus": {},
    }
    for name, b0 in before.items():
        b1 = after.get(name, {})
        d = {k: (b1.get(k, 0) or 0) - (b0.get(k, 0) or 0)
             for k in ("frames", "copied", "parse_ms", "reads", "slot_full")}
        d["us_per_frame"] = round(d["parse_ms"] * 1000.0 / d["frames"], 2) if d["frames"] else None
        d["copied_per_frame"] = round(d["copied"] / d["frames"], 1) if d["frames"] else None
        res["bus"][name] = d

    if args.json:
        print(json.dumps(res, ensure_ascii=False))
    else:
        print(f"[{res['mode']}] 送 {res['sent']}、回 {res['replied']}、{res['elapsed_s']}s、"
              f"{res['frames_per_s']} 幀/s")
        r = res["rtt_ms"]
        print(f"RTT  p50 {r['p50']} / p99 {r['p99']} / max {r['max']} / mean {r['mean']} ms")
        for name, v in res["bus"].items():
            print(f"bus {name}: +{v['frames']} frames, copied {v['copied']}"
                  f" ({v['copied_per_frame']} B/frame), parse {v['parse_ms']} ms"
                  f"（{v['us_per_frame']} us/幀）、slot_full +{v['slot_full']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
