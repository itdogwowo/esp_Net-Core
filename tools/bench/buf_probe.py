#!/usr/bin/env python3
"""緩衝區貼身化的量測工具（host 端）。

每一項尺寸都要有裝置側的數字支撐，這支就是蒐集那些數字的工具：

    buf_probe.py --port /dev/cu.usbmodem* snap                 # 一次讀回 net/rt/mem/log
    buf_probe.py --port ... rtt --n 200                        # 小指令 RTT（p50/p95/max）
    buf_probe.py --port ... uart --frames 50 --size 64         # UART 迴路（0xF00B/0xF00C）
    buf_probe.py --port ... txbench --bytes 1048576 --chunk 4096   # 裝置→主機 CDC 吞吐
    buf_probe.py --port ... storm --n 200 --size 64            # 連發不等待（量 slot 峰值）
    buf_probe.py --port ... idle --seconds 10                  # 閒置對照組

裝置側回報的欄位（STATUS keys=net）：
    reads / max_read   一次讀／搬的最大 bytes       → slot_size 下限
    peak               同時佔用格數的歷史峰值        → slots 下限
    slot_full          拿不到格的次數               → 出現就代表 slots 不夠
    max_dlen / max_ok  收過的最大 payload（含被丟的）→ parser region 下限
    tx_max             回覆最大 bytes               → TX ring／FIFO 下限
    hwm                producer 任務棧剩餘最少 bytes → stack 下限
"""
import argparse
import json
import statistics
import struct
import sys
import time

import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)) + "/..")
from nc4_demux import ADDR_BROADCAST, pack, str_u16, Reader  # noqa: E402

CMD_STATUS_GET = 0x1101
CMD_STATUS_RSP = 0x1102
CMD_DEV_RSP = 0xF002          # 測試指令的通用 JSON 回覆
CMD_HUB_RSP = 0xF004
CMD_UART_STREAM = 0xF00E   # 由獨立 task 灌 UART（唔揸 decoder 鎖）
CMD_LOOP_STAT = 0xF00C
CMD_LOOP_PING = 0xF0FE
CMD_CDC_TX_BENCH = 0xF00D
CMD_TX_BENCH = 0xF007

STATUS_KEYS = ["sys", "net", "rt", "mem", "log", "storage", "cfg"]


def status(reader, ser, key: str, timeout: float = 3.0):
    ser.write(pack(CMD_STATUS_GET, str_u16(key), ADDR_BROADCAST))
    f = reader.wait_for({CMD_STATUS_RSP}, timeout)
    if f is None:
        return None
    payload = f[1]
    n = struct.unpack_from("<H", payload, 0)[0]
    text = payload[2:2 + n].decode("utf-8", "replace")
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        return {"_raw": text}


def snap(reader, ser):
    out = {}
    for k in STATUS_KEYS:
        v = status(reader, ser, k)
        out[k] = v
        time.sleep(0.05)
    return out


def cmd_json(reader, ser, cmd: int, payload: bytes, reply: int, timeout: float = 5.0):
    ser.write(pack(cmd, payload, ADDR_BROADCAST))
    f = reader.wait_for({reply}, timeout)
    if f is None:
        return None
    try:
        n = struct.unpack_from("<H", f[1], 0)[0]
        return json.loads(f[1][2:2 + n].decode("utf-8", "replace"))
    except (struct.error, json.JSONDecodeError):
        return {"_raw": f[1].hex()}


def do_snap(args, reader, ser):
    s = snap(reader, ser)
    print(json.dumps(s, indent=2, ensure_ascii=False))
    return s


def do_rtt(args, reader, ser):
    times = []
    lost = 0
    for _ in range(args.n):
        t0 = time.perf_counter()
        v = status(reader, ser, args.key, timeout=2.0)
        dt = (time.perf_counter() - t0) * 1000.0
        if v is None:
            lost += 1
        else:
            times.append(dt)
    if not times:
        print("全部失敗")
        return None
    times.sort()
    r = {
        "n": len(times),
        "lost": lost,
        "p50_ms": round(statistics.median(times), 3),
        "p95_ms": round(times[int(len(times) * 0.95) - 1], 3),
        "max_ms": round(times[-1], 3),
        "min_ms": round(times[0], 3),
    }
    print(json.dumps(r))
    return r


def do_uart(args, reader, ser):
    payload = struct.pack("<HH", args.frames, args.size)
    t0 = time.perf_counter()
    r = cmd_json(reader, ser, CMD_UART_STREAM, payload, CMD_HUB_RSP, timeout=10.0)
    if r is None:
        print("UART_STREAM 沒有回應")
        return None
    got = -1
    sent = -1
    stable = 0
    deadline = time.time() + args.timeout
    while time.time() < deadline:
        st = cmd_json(reader, ser, CMD_LOOP_STAT, b"", CMD_HUB_RSP, timeout=3.0)
        if st is None:
            break
        got = st.get("rx", -1)
        sent = st.get("sent", -1)
        if got >= args.frames:
            break
        if sent >= args.frames:
            stable += 1
            if stable >= 6:          # 寫完了但 rx 不再增加 → 定案
                break
        time.sleep(0.005)            # 5 ms：50 ms 的輪詢會把時間量化成 50 ms 一格
    dt = (time.perf_counter() - t0) * 1000.0
    res = {
        "frames": args.frames,
        "size": args.size,
        "sent": sent,
        "rx": got,
        "lost": max(0, args.frames - got) if got >= 0 else -1,
        "ms": round(dt, 1),
        "frames_per_s": round(got / (dt / 1000.0), 1) if dt > 0 else 0,
    }
    print(json.dumps(res))
    return res


def do_txbench(args, reader, ser):
    payload = struct.pack("<IHB", args.bytes, args.chunk, args.mode)
    t0 = time.perf_counter()
    r = cmd_json(reader, ser, CMD_CDC_TX_BENCH, payload, CMD_DEV_RSP, timeout=60.0)
    dt = (time.perf_counter() - t0) * 1000.0
    if r is None:
        print("CDC_TX_BENCH 沒有回應")
        return None
    res = {"bench": r, "wall_ms": round(dt, 1)}
    print(json.dumps(res))
    return res


def do_storm(args, reader, ser):
    """連發 N 幀（不等回覆），量裝置側的 slot 峰值與丟幀。"""
    payload = (args.fill.encode() * (args.size // len(args.fill) + 1))[:args.size] if args.size else b""
    one = pack(args.cmd, payload, ADDR_BROADCAST)
    frames = one * args.n
    base = -1
    if args.cmd == CMD_LOOP_PING:
        st0 = cmd_json(reader, ser, CMD_LOOP_STAT, b"", CMD_HUB_RSP, timeout=3.0)
        base = (st0 or {}).get("rx", -1)
    t0 = time.perf_counter()
    # 分段寫，避免一次寫爆 host 端緩衝
    for i in range(0, len(frames), 4096):
        ser.write(frames[i:i + 4096])
    writes = time.perf_counter() - t0
    # 收齊回覆（有的指令不回）
    replies = 0
    done = -1
    deadline = time.time() + args.timeout
    while time.time() < deadline:
        f = reader.poll(0.02)
        if f is not None:
            replies += 1
        if args.cmd == CMD_LOOP_PING:
            # 0xF0FE 不回覆：用 0xF00C 的 rx 計數判斷「裝置收完未」
            st = cmd_json(reader, ser, CMD_LOOP_STAT, b"", CMD_HUB_RSP, timeout=3.0)
            if st is not None:
                done = st.get("rx", -1) - max(base, 0)
                if done >= args.n:
                    break
        elif replies >= args.n:
            break
    dt = time.perf_counter() - t0
    res = {
        "sent": args.n,
        "rx": done,
        "replies": replies,
        "write_ms": round(writes * 1000.0, 1),
        "total_ms": round(dt * 1000.0, 1),
        "frames_per_s": round(args.n / dt, 1) if dt > 0 else 0,
    }
    print(json.dumps(res))
    return res


def do_idle(args, reader, ser):
    time.sleep(args.seconds)
    print(json.dumps({"idle_s": args.seconds}))
    return None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=115200)
    sub = ap.add_subparsers(dest="action", required=True)

    sub.add_parser("snap")

    p = sub.add_parser("rtt")
    p.add_argument("--n", type=int, default=100)
    p.add_argument("--key", default="sys")

    p = sub.add_parser("uart")
    p.add_argument("--frames", type=int, default=50)
    p.add_argument("--size", type=int, default=64)
    p.add_argument("--timeout", type=float, default=20.0)

    p = sub.add_parser("txbench")
    p.add_argument("--bytes", type=int, default=1024 * 1024)
    p.add_argument("--chunk", type=int, default=4096)
    p.add_argument("--mode", type=int, default=0)

    p = sub.add_parser("storm")
    p.add_argument("--cmd", type=lambda v: int(v, 0), default=CMD_STATUS_GET)
    p.add_argument("--n", type=int, default=200)
    p.add_argument("--size", type=int, default=0, help="每幀 payload 大小")
    p.add_argument("--fill", default="A", help="payload 填充字元")
    p.add_argument("--timeout", type=float, default=10.0)

    p = sub.add_parser("idle")
    p.add_argument("--seconds", type=float, default=10.0)

    args = ap.parse_args()
    import serial

    # timeout 要細：pyserial 的 read(n) 會等到 n bytes 或 timeout 為止，
    # 用 50 ms 的 timeout 量 RTT 會量到「剛好 50 ms」（這是量測工具自己的延遲）。
    with serial.Serial(args.port, args.baud, timeout=0.001) as ser:
        time.sleep(0.4)                 # 等 TinyUSB CDC 的 DTR 建立
        reader = Reader(ser)
        fn = {
            "snap": do_snap, "rtt": do_rtt, "uart": do_uart,
            "txbench": do_txbench, "storm": do_storm, "idle": do_idle,
        }[args.action]
        r = fn(args, reader, ser)
        return 0 if r is not None else 1


if __name__ == "__main__":
    raise SystemExit(main())
