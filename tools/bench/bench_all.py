#!/usr/bin/env python3
"""一次跑完所有性能測試（P4 乙太網 + NC4）

    tools/bench/bench_all.py                 # 只量現在的設定
    tools/bench/bench_all.py --mode push     # 改成 PUSH + 零複製 → 重開機 → 量
    tools/bench/bench_all.py --mode pull     # 改成 PULL（同核）
    tools/bench/bench_all.py --mode push-slot# PUSH 但走舊的兩次複製
    tools/bench/bench_all.py --mb 16 --json out.json

會依序量：
    A. 純 TCP：下載單/雙、上傳單/雙、全雙工
    B. NC4 下載（PC→裝置，裝置解析）：1K / 4K / 8K，同一個總量
    C. NC4 上傳（裝置打包→PC）：同核 / 分核 × 1K / 4K / 8K
    D. 純 CPU：打包 / 解析（0xF001，不碰網路）
    E. 雙核 hub 交換（0xF003）
    F. 匯流排統計（0xF008）：copied / resync

每個數字都是「同一台機器、同一個時段」量出來的；跨時段比較請重跑。
"""
import argparse
import binascii
import json
import socket
import struct
import sys
import threading
import time

SOF = b"NC"
ADDR = 0xFFFF

CMD_STATUS = 0x1101
CMD_STATUS_RSP = 0x1102
CMD_BENCH_READY = 0x1811
CMD_BENCH_DATA = 0x1812
CMD_BENCH_RESULT = 0x1813
CMD_BENCH_REPORT = 0x1814
CMD_DEV_BENCH = 0xF001
CMD_DEV_RSP = 0xF002
CMD_HUB_BENCH = 0xF003
CMD_HUB_RSP = 0xF004
CMD_TX_BENCH = 0xF007
CMD_BUS_STATS = 0xF008
CMD_BUS_RSP = 0xF009
CMD_NET_TUNE = 0xF00A
CMD_REBOOT = 0x100F

SINK_PORTS = (5000, 5003)
SRC_PORTS = (5001, 5002)
NC4_PORT = 6000


def pack(cmd, payload=b""):
    body = struct.pack("<BHHH", 4, ADDR, cmd, len(payload)) + payload
    return SOF + body + struct.pack("<I", binascii.crc32(body) & 0xFFFFFFFF)


class Nc4:
    """一條 NC4/TCP 連線（裝置一次只服務一條）"""

    def __init__(self, host, timeout=60):
        self.s = socket.create_connection((host, NC4_PORT), timeout=5)
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buf = b""
        self.timeout = timeout
        time.sleep(0.2)

    def send(self, data):
        self.s.sendall(data)

    def recv_until(self, want_cmd, timeout=None, until_bytes=0):
        """等到指定 cmd 的幀；until_bytes>0 時先把那麼多 payload bytes 收完（上傳用）"""
        timeout = timeout or self.timeout
        end = time.time() + timeout
        got_data = 0
        while time.time() < end:
            while len(self.buf) >= 13:
                i = self.buf.find(SOF)
                if i < 0:
                    self.buf = b""
                    break
                if i:
                    self.buf = self.buf[i:]
                if len(self.buf) < 13:
                    break
                ln = struct.unpack_from("<H", self.buf, 7)[0]
                if len(self.buf) < 9 + ln + 4:
                    break
                cmd = struct.unpack_from("<H", self.buf, 5)[0]
                body = self.buf[2:9 + ln]
                crc = struct.unpack_from("<I", self.buf, 9 + ln)[0]
                frame, self.buf = self.buf[:9 + ln + 4], self.buf[9 + ln + 4:]
                if (binascii.crc32(body) & 0xFFFFFFFF) != crc:
                    continue
                if cmd == want_cmd:
                    return frame[9:9 + ln]
                got_data += len(frame)
                if until_bytes and got_data >= until_bytes:
                    # 資料已經收滿，剩下的就是回覆
                    continue
            self.s.settimeout(max(0.05, end - time.time()))
            try:
                d = self.s.recv(262144)
            except socket.timeout:
                break
            if not d:
                break
            self.buf += d
        return None

    def call(self, cmd, payload=b"", rsp_cmd=None, timeout=10):
        self.send(pack(cmd, payload))
        return self.recv_until(rsp_cmd or (cmd | 1), timeout)

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


def jload(data):
    if not data or len(data) < 2:
        return None
    n = struct.unpack_from("<H", data, 0)[0]
    try:
        return json.loads(data[2:2 + n])
    except Exception:
        return None


# ───────────────────────── A. 純 TCP ─────────────────────────

def tcp_download(host, port, mb):
    s = socket.create_connection((host, port), timeout=10)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    chunk = b"x" * 1460
    total = int(mb * 1048576)
    sent = 0
    t0 = time.time()
    while sent < total:
        s.sendall(chunk)
        sent += len(chunk)
    s.shutdown(socket.SHUT_WR)
    try:
        s.recv(1)
    except OSError:
        pass
    dt = time.time() - t0
    s.close()
    return sent / dt / 1048576, dt


def tcp_upload(host, port, mb):
    s = socket.create_connection((host, port), timeout=10)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    total = int(mb * 1048576)
    s.sendall(b"GO %d\n" % total)
    got = 0
    t0 = time.time()
    while got < total:
        d = s.recv(65536)
        if not d:
            break
        got += len(d)
    dt = time.time() - t0
    s.close()
    return got / dt / 1048576, dt


def parallel(fns):
    res = [None] * len(fns)

    def run(i, fn):
        try:
            res[i] = fn()
        except Exception as e:                      # noqa: BLE001
            res[i] = (0.0, 0.0)
            print(f"    (通道 {i} 失敗: {e})", file=sys.stderr)

    ts = [threading.Thread(target=run, args=(i, fn)) for i, fn in enumerate(fns)]
    t0 = time.time()
    for t in ts:
        t.start()
    for t in ts:
        t.join()
    return res, time.time() - t0


# ───────────────────────── B. NC4 下載 ─────────────────────────

def nc4_download(c, size, frames):
    payload = b"\xa5" * size
    frame = pack(CMD_BENCH_DATA, payload)
    c.send(pack(CMD_BENCH_READY))
    c.recv_until(CMD_BENCH_REPORT, 5)
    blob = frame * frames
    t0 = time.time()
    c.s.settimeout(120)
    c.send(blob)
    c.s.settimeout(30)
    c.send(pack(CMD_BENCH_RESULT))
    d = c.recv_until(CMD_BENCH_REPORT, 30)
    dt = time.time() - t0
    got = struct.unpack_from("<I", d, 0)[0] if d and len(d) >= 4 else -1
    total = len(blob)
    return total / dt / 1048576, dt, got, frames


# ───────────────────────── C. NC4 上傳 ─────────────────────────

def nc4_upload(c, size, frames, split):
    total = 13 + size
    want = frames * total
    c.send(pack(CMD_TX_BENCH, struct.pack("<IHB", frames, size, 1 if split else 0)))
    t0 = time.time()
    c.s.settimeout(120)
    # 收到 reply 為止；中間那些 BENCH_DATA 就是上傳的資料
    d = c.recv_until(CMD_HUB_RSP, 120, until_bytes=want)
    dt = time.time() - t0
    rsp = jload(d)
    return rsp, dt


# ───────────────────────── 主流程 ─────────────────────────

def set_mode(host, mode):
    split, core, frame = {
        "pull": (0, 0, 1),
        "push": (1, 1, 1),
        "push-slot": (1, 1, 0),
    }[mode]
    c = Nc4(host, timeout=5)
    r = c.call(CMD_NET_TUNE, bytes([split, core, frame]), CMD_HUB_RSP, 5)
    print("  設定:", jload(r))
    c.send(pack(CMD_REBOOT, struct.pack("<I", 100)))
    c.close()
    time.sleep(1)
    for _ in range(40):
        try:
            s = socket.create_connection((host, NC4_PORT), timeout=0.5)
            s.close()
            c = Nc4(host, timeout=5)
            d = c.call(CMD_STATUS, struct.pack("<H", 3) + b"sys", CMD_STATUS_RSP, 3)
            c.close()
            if d:
                return True
        except OSError:
            pass
        time.sleep(0.5)
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="10.10.1.50")
    ap.add_argument("--mode", choices=["current", "pull", "push", "push-slot"],
                    default="current")
    ap.add_argument("--mb", type=float, default=16)
    ap.add_argument("--json", default="")
    a = ap.parse_args()

    rows = []

    def add(group, name, form, cores, value, unit="MB/s", note=""):
        rows.append(dict(group=group, name=name, form=form, cores=cores,
                         value=value, unit=unit, note=note))
        print(f"  {group:4s} {name:26s} {value:9.2f} {unit:5s}  {note}")

    if a.mode != "current":
        print(f"→ 切到 {a.mode}（寫 NVS + 重開機）")
        if not set_mode(a.host, a.mode):
            print("!! 重開機後連不上", file=sys.stderr)
            return 1

    # 先看裝置現在是什麼模式
    c = Nc4(a.host)
    st = jload(c.call(CMD_BUS_STATS, b"\x00", CMD_BUS_RSP, 5))
    bus = next((b for b in (st or {}).get("bus", []) if b["name"] == "TCP"), {})
    print(f"→ 裝置模式: {bus.get('mode', '?')}（frame={bus.get('frame')}）")
    c.call(CMD_BUS_STATS, b"\x01", CMD_BUS_RSP, 5)      # 歸零
    c.close()

    # ── A ──
    print("\nA. 純 TCP")
    v, _ = tcp_download(a.host, SINK_PORTS[0], a.mb)
    add("A1", "下載（1 條）", "純 TCP", "單核 core1", v)

    (r1, r2), wall = parallel([
        lambda: tcp_download(a.host, SINK_PORTS[0], a.mb),
        lambda: tcp_download(a.host, SINK_PORTS[1], a.mb),
    ])
    add("A2", "下載（2 條）", "純 TCP", "雙核 core1+core0", r1[0] + r2[0],
        note=f"{r1[0]:.2f} + {r2[0]:.2f}")

    v, _ = tcp_upload(a.host, SRC_PORTS[0], a.mb)
    add("A3", "上傳（1 條）", "純 TCP", "單核 core0", v)

    (u1, u2), _ = parallel([
        lambda: tcp_upload(a.host, SRC_PORTS[0], a.mb),
        lambda: tcp_upload(a.host, SRC_PORTS[1], a.mb),
    ])
    add("A4", "上傳（2 條）", "純 TCP", "雙核 core0+core1", u1[0] + u2[0],
        note=f"{u1[0]:.2f} + {u2[0]:.2f}")

    (d, u), _ = parallel([
        lambda: tcp_download(a.host, SINK_PORTS[0], a.mb),
        lambda: tcp_upload(a.host, SRC_PORTS[0], a.mb / 2),
    ])
    add("A5", "全雙工（同時）", "純 TCP", "雙核", d[0] + u[0],
        note=f"下 {d[0]:.2f} + 上 {u[0]:.2f}")

    # ── B ──
    print("\nB. NC4 下載（PC→裝置，裝置解析）")
    c = Nc4(a.host)
    for size, frames in ((1024, 16000), (4096, 4000), (8192, 2000)):
        bw, dt, got, want = nc4_download(c, size, frames)
        add("B", f"下載+解析 {size//1024}K", "NC4", "見模式",
            bw, note=f"{got}/{want} 幀 {dt:.2f}s")
    c.close()

    # ── C ──
    print("\nC. NC4 上傳（裝置打包→PC）")
    c = Nc4(a.host)
    for split in (False, True):
        for size, frames in ((1024, 16000), (4096, 4000), (8192, 2000)):
            rsp, dt = nc4_upload(c, size, frames, split)
            tag = "分核" if split else "同核"
            if rsp is None:
                add("C", f"上傳 {size//1024}K（{tag}）", "NC4", tag, 0, note="失敗")
                continue
            mb = rsp["sent"] * (13 + size) / 1048576
            add("C", f"上傳 {size//1024}K（{tag}）", "NC4", tag, mb / dt,
                note=f"{rsp['sent']}/{rsp['frames']} 幀 {dt:.2f}s")
    c.close()

    # ── D ──
    print("\nD. 純 CPU（0xF001，不碰網路）")
    c = Nc4(a.host)
    for mode, iters, tag in ((0, 5000, "解析 8K"), (1, 20000, "打包 8K")):
        d = c.call(CMD_DEV_BENCH, struct.pack("<HHB", 8192, iters, mode), CMD_DEV_RSP, 60)
        j = jload(d) or {}
        add("D", tag, "CPU", "—", j.get("mb_s", 0), note=f"{j.get('us_per_frame', 0):.1f} us/幀")
    c.close()

    # ── E ──
    print("\nE. 雙核 hub 交換（0xF003）")
    c = Nc4(a.host)
    d = c.call(CMD_HUB_BENCH, struct.pack("<I", 20000), CMD_HUB_RSP, 30)
    j = jload(d) or {}
    add("E", "hub 交換 20k", "hub", "core0+core1", j.get("per_sec", 0), unit="/s",
        note=f"{j.get('us', 0)/1000:.1f} ms, torn={j.get('torn')}")
    c.close()

    # ── F ──
    c = Nc4(a.host)
    st = jload(c.call(CMD_BUS_STATS, b"\x00", CMD_BUS_RSP, 5)) or {}
    c.close()
    b = next((x for x in st.get("bus", []) if x["name"] == "TCP"), {})
    rows.append(dict(group="F", name="bus stats", form=b.get("mode", "?"),
                     cores=f"frame={b.get('frame')}", value=float(b.get("mb", 0)),
                     unit="MB", note=f"copied={b.get('copied')} resync={st.get('resync')}"))
    print(f"  F    {b.get('mode')}: 收 {b.get('mb')} MB, copied={b.get('copied')} B, "
          f"resync={st.get('resync')}, {b.get('us_per_frame')} us/幀")

    print("\n=== 完整表 ===")
    print(f"{'#':3s} {'項目':28s} {'形態':6s} {'核心':16s} {'數值':>10s}  備註")
    for r in rows:
        print(f"{r['group']:3s} {r['name']:28s} {r['form']:6s} {r['cores']:16s} "
              f"{r['value']:10.2f}  {r['note']}")

    if a.json:
        with open(a.json, "w") as f:
            json.dump({"mode": bus.get("mode"), "rows": rows}, f,
                      ensure_ascii=False, indent=2)
        print(f"\n→ 寫入 {a.json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
