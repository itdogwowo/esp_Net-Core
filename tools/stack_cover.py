#!/usr/bin/env python3
"""把 decode 任務的棧「走遍」：逐條打已註冊指令，再讀 HWM。

為什麼要這樣做
--------------
棧深是靜態可算的（呼叫圖明確），但這份程式的 decode 路徑有一段是**函數指標**：
    entry.handler(ctx, f)     ← handler 表，GCC 的 .ci 只見到 __indirect_call
所以靜態工具算不到那一段（實測靜態 1,408 B，實機 2,944 B）。

指令表是**有限而且可列舉**的（STATUS keys=cmds），所以正確做法不是「希望有跑到」，
而是「逐條跑一次」。跑完之後 HWM 就覆蓋全部 handler，不再是抽樣。

用法
----
    tools/stack_cover.py --port /dev/cu.usbmodem58E6C569D5F41
    tools/stack_cover.py --port ... --dry-run        # 只印會打哪幾條

危險指令預設跳過（重啟／OTA／格式化／SD 抹寫／壓力測試）；`--all` 才會全打。
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nc4_demux import parse_frames, pack  # noqa: E402

STATUS_GET = 0x1101
STATUS_RSP = 0x1102

# 唔可以亂打的：會重啟、寫 flash、抹卡、跑幾十秒、或者同一個 static 被打兩次
# 0xF0xx 整段是測試指令（bench / format / SD 抹寫），0x18xx 是迴路測試。
# 實測教訓：0xF003 HUB_BENCH 會開兩個 task 跑 3 秒；主機在它未完之前再打一條，
# 就等於同一個 static 狀態被兩邊同時用 → 板子直接重啟。所以要整段跳。
def deny_reason(c: int) -> str | None:
    if c == 0x100F:
        return "REBOOT"
    if 0x2200 <= c <= 0x22FF:
        # 只讀查詢安全（VERSION / CAPS / PROGRESS / STATE）；其餘（BEGIN/WRITE/END/APPLY）跳過
        if c in (0x2206, 0x2210, 0x2218, 0x221A):
            return None
        return "OTA"
    if 0xF000 <= c <= 0xF0FF:
        return "測試/bench（會跑幾秒、會寫卡）"
    if 0x1800 <= c <= 0x18FF:
        return "迴路測試"
    return None


def read_frames(s, want_cmd, timeout=1.5):
    dm_buf = b""
    end = time.time() + timeout
    while time.time() < end:
        chunk = s.read(4096)
        if not chunk:
            continue
        dm_buf += chunk
        frames, rest = parse_frames(dm_buf)
        dm_buf = rest
        for c, d in frames:
            if c == want_cmd:
                return d
    return None


def status(s, keys: str) -> dict | None:
    payload = struct.pack("<H", len(keys)) + keys.encode()
    s.reset_input_buffer()
    s.write(pack(STATUS_GET, payload))
    d = read_frames(s, STATUS_RSP, 2.5)
    if d is None:
        return None
    n = struct.unpack_from("<H", d)[0]
    try:
        return json.loads(d[2:2 + n].decode("utf-8", "replace"))
    except json.JSONDecodeError:
        return None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--all", action="store_true", help="連危險指令都打（後果自負）")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    import serial

    s = serial.Serial(args.port, args.baud, timeout=0.1)
    time.sleep(0.5)
    s.reset_input_buffer()

    st = status(s, "cmds")
    if not st or "cmds" not in st:
        print("拿不到指令表（STATUS keys=cmds）", file=sys.stderr)
        return 1
    cmds = st["cmds"]

    def hwm():
        r = status(s, "rt") or {}
        out = {}
        for t in r.get("rt", []):
            if t["n"].startswith("decode"):
                out[t["n"]] = t["hwm"]
        return out

    base = hwm()
    print(f"指令表 {len(cmds)} 條；起始 {' '.join(f'{k}={v}' for k, v in base.items())}\n")

    ran = skipped = 0
    for item in cmds:
        c, owner = int(item["c"]), item.get("o", "-")
        name = deny_reason(c)
        if name and not args.all:
            print(f"  skip 0x{c:04X}  {owner:>5}  ({name})")
            skipped += 1
            continue
        if args.dry_run:
            print(f"  would 0x{c:04X}  {owner}")
            continue
        print(f"  → 0x{c:04X} {owner}", flush=True)
        s.write(pack(c, b""))          # 空 payload：只為走一次 handler
        time.sleep(0.25)
        ran += 1

    final = hwm()
    print(f"\n打了 {ran} 條、跳過 {skipped} 條")
    for k, v in sorted(final.items()):
        print(f"  {k}: 剩 {v} B（HWM，覆蓋上面全部指令）")
    if skipped and not args.all:
        print("\n注：跳過的包含 OTA / SD 抹寫 / 壓力測試 —— 那幾條的棧深要用 --all 或單獨測。")
    s.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
