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


def _round_up(v: int, step: int) -> int:
    return ((v + step - 1) // step) * step


def do_sizing(args, reader, ser):
    """把裝置側的峰值換算成「貼身尺寸建議」；用來決定 config，而不是憑感覺。

    region 最少要放得落「一幀」＝ 9(header) + payload + 4(crc)；
    slot_size ≥ 實測單次最大搬運；slots ≥ 峰值 + 1（留一格給 producer 週轉）。
    """
    net = status(reader, ser, "net")
    rt = status(reader, ser, "rt")
    if net is None:
        print("讀不到 net")
        return None
    # 已知但這次沒有打出來的需求（例如 CDC 要收 OTA 的 4 KB chunk）：
    # 只靠「觀察到的最大值」會把 region 算得太細，所以要用 --declare 補。
    declared = {}
    for item in args.declare or []:
        name, _, val = item.partition("=")
        try:
            declared[name] = int(val)
        except ValueError:
            sys.exit(f"--declare 格式要 BUS=BYTES，收到 {item!r}")
    if not declared:
        print("# 注意：只用觀察到的流量；已知但未發生的需求要用 --declare BUS=BYTES 補",
              file=sys.stderr)
    rows = []
    for b in net.get("net", {}).get("bus", []):
        name = b.get("name")
        need = max(int(b.get("max_dlen") or 0), declared.get(name, 0))
        frame_bytes = 13 + need
        rows.append({
            "bus": name,
            "declared_dlen": declared.get(name),
            "region_now": b.get("region"),
            "region_rec": _round_up(max(frame_bytes, 256), 256),
            "max_dlen": b.get("max_dlen"),
            "region_fits_frame": b.get("region", 0) >= frame_bytes,
            "max_read": b.get("max_read"),
            "slot_rec": _round_up(max(int(b.get("max_read") or 0), 16), 128),
            "peak": b.get("peak"),
            "slots_rec": max(2, int(b.get("peak") or 0) + 1),
            "slot_full": b.get("slot_full"),
            "tx_max": b.get("tx_max"),
        })
    stacks = {}
    if rt is not None:
        for t in rt.get("rt", []):
            stacks[t["n"]] = t["hwm"]
    print(json.dumps({"bus": rows, "stack_hwm": stacks}, indent=2, ensure_ascii=False))
    return rows


def _uart_run(reader, ser, frames: int, size: int, timeout: float = 30.0):
    """0xF00E 串流 → 等 rx 到齊 → 回 {frames,size,ms,frames_per_s,lost}。"""
    s_payload = struct.pack("<HH", frames, size)
    t0 = time.perf_counter()
    r = cmd_json(reader, ser, CMD_UART_STREAM, s_payload, CMD_HUB_RSP, timeout=10.0)
    if r is None:
        return {"frames": frames, "size": size, "ms": None,
                "frames_per_s": None, "lost": -1}
    got, sent, stable = -1, -1, 0
    deadline = time.time() + timeout
    while time.time() < deadline:
        st = cmd_json(reader, ser, CMD_LOOP_STAT, b"", CMD_HUB_RSP, timeout=3.0)
        if st is None:
            break
        got, sent = st.get("rx", -1), st.get("sent", -1)
        if got >= frames:
            break
        if sent >= frames:
            stable += 1
            if stable >= 6:
                break
        time.sleep(0.005)
    dt = (time.perf_counter() - t0) * 1000.0
    return {"frames": frames, "size": size, "ms": round(dt, 1),
            "frames_per_s": round(got / (dt / 1000.0), 1) if dt > 0 and got > 0 else None,
            "lost": max(0, frames - got) if got >= 0 else -1}


def _storm_run(reader, ser, n: int, size: int, timeout: float = 40.0):
    base = cmd_json(reader, ser, CMD_LOOP_STAT, b"", CMD_HUB_RSP, timeout=3.0) or {}
    base_rx = base.get("rx", 0)
    payload = (b"A" * size) if size else b""
    one = pack(CMD_LOOP_PING, payload, ADDR_BROADCAST)
    buf = one * n
    t0 = time.perf_counter()
    for i in range(0, len(buf), 4096):
        ser.write(buf[i:i + 4096])
    write_ms = (time.perf_counter() - t0) * 1000.0
    done = -1
    deadline = time.time() + timeout
    while time.time() < deadline:
        st = cmd_json(reader, ser, CMD_LOOP_STAT, b"", CMD_HUB_RSP, timeout=3.0)
        if st is None:
            break
        done = st.get("rx", 0) - base_rx
        if done >= n:
            break
        time.sleep(0.005)
    total_ms = (time.perf_counter() - t0) * 1000.0
    return {"n": n, "size": size, "rx": done, "lost": max(0, n - done),
            "write_ms": round(write_ms, 1), "total_ms": round(total_ms, 1),
            "frames_per_s": round(n / (total_ms / 1000.0), 1) if total_ms > 0 else None}


def do_session(args, reader, ser):
    """跑一遍標準量測（可重複）：snap → rtt → UART 階梯 → CDC TX → 洪水 → 峰值。"""
    out = {"when": time.strftime("%Y-%m-%dT%H:%M:%S"), "port": args.port,
           "phase": args.label or "?", "results": {}}
    res = out["results"]

    # 0xF008 reset=1：把統計窗口歸零，否則 before/after 會把上一個窗口的數字帶進來。
    # reset 只在測試映像有（release 沒有 0xF008）；失敗就照樣量，但標記出來。
    if not args.no_reset:
        ser.write(pack(0xF008, b"\x01", ADDR_BROADCAST))
        ok = reader.wait_for({0xF009}, 2.0) is not None
        res["stats_reset"] = bool(ok)

    sysm = status(reader, ser, "sys") or {}
    res["app"] = sysm.get("sys", {}).get("app")
    res["heap_internal"] = sysm.get("sys", {}).get("heap")

    rtt_runs = []
    for _ in range(3):
        times = []
        for _ in range(max(4, args.rtt // 3)):
            t0 = time.perf_counter()
            if status(reader, ser, "sys", timeout=2.0) is not None:
                times.append((time.perf_counter() - t0) * 1000.0)
        if times:
            times.sort()
            rtt_runs.append(round(times[len(times) // 2], 3))
    res["rtt_p50_ms"] = min(rtt_runs) if rtt_runs else None

    res["uart"] = []
    for spec in args.uart or []:
        try:
            frames_s, size_s = spec.split(":")
            res["uart"].append(_uart_run(reader, ser, int(frames_s), int(size_s)))
        except ValueError:
            sys.exit(f"--uart 要 frames:size，收到 {spec!r}")

    if args.txbench:
        payload = struct.pack("<IHB", args.txbench, 4096, 0)
        t0 = time.perf_counter()
        r = cmd_json(reader, ser, CMD_CDC_TX_BENCH, payload, CMD_DEV_RSP, timeout=60.0)
        wall = time.perf_counter() - t0
        res["cdc_tx"] = {"bytes": args.txbench, "wall_s": round(wall, 3),
                         "mb_s": round(args.txbench / wall / (1024 * 1024), 3) if wall > 0 else None,
                         "bench": r}

    res["storms"] = []
    for spec in args.storm or []:
        try:
            n_s, size_s = spec.split(":")
            res["storms"].append(_storm_run(reader, ser, int(n_s), int(size_s)))
        except ValueError:
            sys.exit(f"--storm 要 frames:size，收到 {spec!r}")

    net = status(reader, ser, "net") or {}
    rt = status(reader, ser, "rt") or {}
    res["bus"] = [{k: b.get(k) for k in
                   ("name", "core", "region", "max_read", "peak", "slot_full",
                    "crc_err", "bad_hdr", "remote", "max_dlen")}
                  for b in (net.get("net", {}).get("bus") or [])]
    res["stack_hwm"] = {t["n"]: t["hwm"] for t in (rt.get("rt") or [])}

    if args.out:
        with open(args.out, "w", encoding="utf-8") as fh:
            fh.write(json.dumps(out, indent=2, ensure_ascii=False) + "\n")
        print(f"→ {args.out}", file=sys.stderr)
    print(json.dumps(out, indent=2, ensure_ascii=False))
    return out


def _flat_metrics(d: dict) -> dict:
    """把 session JSON 攤平成 name → 數值（compare 用）。"""
    r = d.get("results", d)
    m = {}
    if r.get("rtt_p50_ms") is not None:
        m["rtt_p50_ms"] = r["rtt_p50_ms"]
    if r.get("heap_internal"):
        m["heap_internal"] = r["heap_internal"]
    for i, u in enumerate(r.get("uart") or []):
        if u.get("frames_per_s"):
            m[f"uart_{u['frames']}x{u['size']}_fps"] = u["frames_per_s"]
        m[f"uart_{i}_lost"] = u.get("lost")
    if (r.get("cdc_tx") or {}).get("mb_s"):
        m["cdc_tx_mb_s"] = r["cdc_tx"]["mb_s"]
    for i, s in enumerate(r.get("storms") or []):
        if s.get("frames_per_s"):
            m[f"storm_{s['n']}x{s['size']}_fps"] = s["frames_per_s"]
        m[f"storm_{i}_lost"] = s.get("lost")
    for b in r.get("bus") or []:
        n = b.get("name")
        m[f"{n}_max_read"] = b.get("max_read")
        m[f"{n}_peak"] = b.get("peak")
        m[f"{n}_slot_full"] = b.get("slot_full")
        m[f"{n}_crc"] = b.get("crc_err")
    for k, v in (r.get("stack_hwm") or {}).items():
        m[f"hwm_{k}"] = v
    return m


def do_compare(args, reader, ser):
    """兩個 session 檔案並排；吞吐/延遲退步 > tolerance% 就標 ⚠。"""
    a = json.load(open(args.before, encoding="utf-8"))
    b = json.load(open(args.after, encoding="utf-8"))
    ma, mb = _flat_metrics(a), _flat_metrics(b)
    tol = args.tolerance
    print(f"{'metric':30s} {'before':>12s} {'after':>12s} {'delta':>10s}")
    worse = 0
    for k, va in ma.items():
        vb = mb.get(k)
        if va is None or vb is None:
            continue
        if isinstance(va, (int, float)) and isinstance(vb, (int, float)):
            pct = (vb - va) / abs(va) * 100.0 if va else 0.0
            bad = False
            if "fps" in k or "mb_s" in k or "heap" in k:
                bad = pct < -tol                       # 越高越好
            elif "_ms" in k:
                bad = pct > tol                        # 越低越好
            elif "lost" in k or "slot_full" in k or "crc" in k:
                bad = vb > va                          # 一增加就標
            worse += 1 if bad else 0
            print(f"{k:30s} {va:12.2f} {vb:12.2f} {pct:+9.1f}%{'  ⚠' if bad else ''}")
    print(f"\n退步項：{worse}（門檻 {tol}%；lost/slot_full/crc 只要增加就標）")
    return worse


def do_idle(args, reader, ser):
    time.sleep(args.seconds)
    print(json.dumps({"idle_s": args.seconds}))
    return None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", help="序列埠（compare 不需要）")
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

    p = sub.add_parser("sizing")
    p.add_argument("--declare", action="append", default=[],
                   help="已知需求 BUS=PAYLOAD_BYTES，可重複（例：--declare CDC=4096）")

    p = sub.add_parser("session", help="跑一遍標準量測（貼身化的 before/after）")
    p.add_argument("--out", help="把結果寫成 JSON（之後可以 compare）")
    p.add_argument("--label", help="這輪的標籤（before / after-uart-1k …）")
    p.add_argument("--no-reset", action="store_true",
                   help="不先清 BUS_STATS（release 映像沒有 0xF008）")
    p.add_argument("--rtt", type=int, default=30, help="RTT 樣本數（3 組取中位數再取最小）")
    p.add_argument("--uart", action="append", help="UART 階梯 frames:size，可重複")
    p.add_argument("--storm", action="append", help="CDC 洪水 frames:size，可重複")
    p.add_argument("--txbench", type=int, default=1048576, help="CDC TX 位元組數（0=跳過）")
    p.set_defaults(fn=None)

    p = sub.add_parser("compare", help="並排兩個 session 結果")
    p.add_argument("--before", required=True)
    p.add_argument("--after", required=True)
    p.add_argument("--tolerance", type=float, default=3.0, help="吞吐/延遲容許退步百分比")
    p.set_defaults(fn=None)

    p = sub.add_parser("idle")
    p.add_argument("--seconds", type=float, default=10.0)

    args = ap.parse_args()
    import serial

    # timeout 要細：pyserial 的 read(n) 會等到 n bytes 或 timeout 為止，
    # 用 50 ms 的 timeout 量 RTT 會量到「剛好 50 ms」（這是量測工具自己的延遲）。
    if args.action == "compare":
        return do_compare(args, None, None)
    if not args.port:
        sys.exit("--port 是必需的（compare 除外）")

    with serial.Serial(args.port, args.baud, timeout=0.001) as ser:
        time.sleep(0.4)                 # 等 TinyUSB CDC 的 DTR 建立
        reader = Reader(ser)
        fn = {
            "snap": do_snap, "rtt": do_rtt, "uart": do_uart,
            "txbench": do_txbench, "storm": do_storm, "idle": do_idle,
            "sizing": do_sizing, "session": do_session, "compare": do_compare,
        }[args.action]
        r = fn(args, reader, ser)
        return 0 if r is not None else 1


if __name__ == "__main__":
    raise SystemExit(main())
