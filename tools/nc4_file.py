#!/usr/bin/env python3
"""FAT 讀取與儲存狀態。

    tools/nc4_file.py --port /dev/cu.usbmodem* status          # STATUS_GET storage
    tools/nc4_file.py --port /dev/cu.usbmodem* query /config.json
    tools/nc4_file.py --port /dev/cu.usbmodem* claim           # STATUS_SET file_mode=1
    tools/nc4_file.py --port /dev/cu.usbmodem* release         # STATUS_SET file_mode=0
    tools/nc4_file.py --port /dev/cu.usbmodem* set '{"System":{"fs_format":1}}' 1
        # STATUS_SET：寫 NVS 旗標；之後送 REBOOT，下次開機才會抹 data 分區＋重建 FAT

讀取（FILE_QUERY）在任何模式都答得到：
  主機擁有 FAT 時走唯讀視圖（`/ro`），答案可能過期或半截（回覆 pending=1）；
  裝置接管時走 `/data`，是真值（pending=0）。

只有寫入才需要 claim。設計見 docs/03_notes/fat-ownership.md。
"""
from __future__ import annotations

import argparse
import binascii
import json
import struct
import sys
import time

import os

# 協議實作只有一份：tools/nc4_demux.py（嚴格 CRC、失敗只前進 1 byte）
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nc4_demux import parse_frames, pack  # noqa: E402

STATUS_GET = 0x1101
STATUS_RSP = 0x1102
STATUS_SET = 0x1103
STATUS_ACK = 0x1104
FILE_QUERY = 0x2005
FILE_QUERY_RSP = 0x2006
FILE_ERROR_RSP = 0x2010


def frames(buf: bytes):
    """只取完整的幀（其餘留在緩衝外的部分丟棄——回覆都是短幀）。"""
    return parse_frames(buf)[0]


def str_u16len(b: bytes, off: int = 0) -> tuple[str, int]:
    n = b[off] | (b[off + 1] << 8)
    return b[off + 2:off + 2 + n].decode("utf-8", "replace"), off + 2 + n


def enc_str(s: str) -> bytes:
    raw = s.encode()
    return struct.pack("<H", len(raw)) + raw


def parse_query(d: bytes) -> dict:
    exists = d[0]
    sha = d[1:33].hex()
    size = struct.unpack_from("<I", d, 33)[0]
    path, off = str_u16len(d, 37)
    free = struct.unpack_from("<I", d, off)[0]
    return {"exists": exists, "sha256": sha, "size": size, "path": path,
            "free": free, "pending": d[off + 4]}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=115200)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("status")
    sub.add_parser("claim")
    sub.add_parser("release")
    st = sub.add_parser("set")        # STATUS_SET：任意 JSON + persist
    st.add_argument("json")
    st.add_argument("persist", type=int, nargs="?", default=1)
    q = sub.add_parser("query")
    q.add_argument("path")
    args = ap.parse_args()

    import serial

    if args.cmd == "query":
        cmd, payload = FILE_QUERY, enc_str(args.path)
        wait = 1.2
    elif args.cmd == "status":
        cmd, payload = STATUS_GET, enc_str("storage")
        wait = 1.0
    elif args.cmd == "set":
        cmd, payload = STATUS_SET, enc_str(args.json) + bytes([args.persist & 0xFF])
        wait = 2.0
    else:                              # claim / release
        mode = 1 if args.cmd == "claim" else 0
        body = json.dumps({"System": {"file_mode": mode}}, separators=(",", ":"))
        cmd, payload = STATUS_SET, enc_str(body) + bytes([0])
        wait = 2.0

    with serial.Serial(args.port, args.baud, timeout=0.2) as s:
        time.sleep(0.6)
        s.reset_input_buffer()
        s.write(pack(cmd, payload))
        got = []
        try:
            if args.cmd in ("claim", "release"):
                # 拆掉/建立 LUN 要等主機做完手上的 I/O（macOS 在索引時可以幾秒），
                # 固定 2 秒會 miss 掉 ACK —— 等足 10 秒或者見到 ACK 為止。
                deadline = time.time() + 10.0
                buf = b""
                while time.time() < deadline:
                    chunk = s.read(4096)
                    if not chunk:
                        continue
                    buf += chunk
                    got = frames(buf)
                    if any(c == STATUS_ACK for c, _ in got):
                        break
            else:
                time.sleep(wait)
                got = frames(s.read(16384))
        except Exception:
            got = []      # release 會觸發 USB 重新列舉，連線可能先斷

    if not got:
        if args.cmd == "release":
            print("release 已送出（USB 重新列舉中，約 1 秒後主機的碟會回來）")
            return 0
        print("沒有回覆")
        return 1

    rc = 0
    for c, d in got:
        if c == STATUS_RSP:
            print(str_u16len(d)[0])
        elif c == STATUS_ACK:
            ok = d[0]
            msg = str_u16len(d, 1)[0]
            print(f"{'OK' if ok else 'FAIL'}  {msg}")
            rc = 0 if ok else 1
        elif c == FILE_QUERY_RSP:
            r = parse_query(d)
            print(f"exists   {r['exists']}")
            if r["exists"]:
                print(f"size     {r['size']} bytes")
                print(f"sha256   {r['sha256']}")
            print(f"free     {r['free']} bytes ({r['free'] / 1024:.0f} KiB)")
            print(f"path     {r['path']}")
            print("pending  1（唯讀視圖，主機可能正在改）" if r["pending"] else "pending  0（裝置接管，真值）")
        elif c == FILE_ERROR_RSP:
            busy = d[6]
            print(f"錯誤：busy={busy}" + ("（讀不到 FAT）" if busy else ""))
            rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
