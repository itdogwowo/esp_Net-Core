#!/usr/bin/env python3
"""FAT 讀取與儲存狀態。

    tools/nc4_file.py --port /dev/cu.usbmodem* status          # STATUS_GET storage
    tools/nc4_file.py --port /dev/cu.usbmodem* query /config.json
    tools/nc4_file.py --port /dev/cu.usbmodem* claim           # STATUS_SET file_mode=1
    tools/nc4_file.py --port /dev/cu.usbmodem* release         # STATUS_SET file_mode=0
    tools/nc4_file.py --port /dev/cu.usbmodem* put <local> <remote>   # 上傳檔案（FILE 0x2001–0x2003）
    tools/nc4_file.py --port /dev/cu.usbmodem* get <remote> <local>   # 下載檔案（FILE_READ 0x2007）
    tools/nc4_file.py --port /dev/cu.usbmodem* delete <remote>        # 刪檔（0x2009）
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
import hashlib
import json
import os
import random
import struct
import sys
import time

import os

# 協議實作只有一份：tools/nc4_demux.py（嚴格 CRC、失敗只前進 1 byte）
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nc4_demux import Reader, parse_frames, pack  # noqa: E402

STATUS_GET = 0x1101
STATUS_RSP = 0x1102
STATUS_SET = 0x1103
STATUS_ACK = 0x1104
FILE_BEGIN = 0x2001
FILE_CHUNK = 0x2002
FILE_END = 0x2003
FILE_ACK = 0x2004
FILE_READ = 0x2007
FILE_CHUNK_RSP = FILE_CHUNK   # READ 的回覆都係 0x2002
FILE_QUERY = 0x2005
FILE_QUERY_RSP = 0x2006
FILE_CONFIRM = 0x2008
FILE_DELETE = 0x2009
FILE_UNDO = 0x200A
FILE_ERROR_RSP = 0x2010

FILE_ERR_NAMES = ["err_no_space", "err_write_fail", "err_offset_mismatch", "err_id_mismatch",
                  "err_sha_mismatch", "err_not_active", "err_busy"]


def _enc_str(s: str) -> bytes:
    b = s.encode()
    return struct.pack("<H", len(b)) + b


def _dec_str(p: bytes, off: int = 0) -> str:
    if len(p) < off + 2:
        return ""
    n = struct.unpack_from("<H", p, off)[0]
    return p[off + 2:off + 2 + n].decode("utf-8", "replace")


def _err_name(p: bytes) -> str:
    names = [n for i, n in enumerate(FILE_ERR_NAMES) if i < len(p) and p[i]]
    failed = struct.unpack_from("<I", p, 7)[0] if len(p) >= 11 else 0
    written = struct.unpack_from("<I", p, 11)[0] if len(p) >= 15 else 0
    path = _dec_str(p, 15) if len(p) >= 17 else ""
    return f"{'/'.join(names) or 'err'}" + (f" failed={failed}" if failed else "") + \
           (f" written={written}" if written else "") + (f" path={path}" if path else "")


def _wait(r: Reader, cmds: set[int], timeout: float):
    f = r.wait_for(cmds, timeout)
    if f is None:
        return None
    return f


def cmd_get(port: str, baud: int, remote: str, local: str, chunk: int = 4096) -> int:
    """FILE_READ（0x2007）：分塊下載直到 short read；回覆 0x2002（off u32 + data）。"""
    import serial
    data = bytearray()
    with serial.Serial(port, baud, timeout=0.2) as ser:
        time.sleep(0.6)
        ser.reset_input_buffer()
        r = Reader(ser)
        off = 0
        while True:
            payload = _enc_str(remote) + struct.pack("<IH", off, chunk)
            ser.write(pack(FILE_READ, payload))
            f = _wait(r, {FILE_CHUNK_RSP, FILE_ERROR_RSP}, 8.0)
            if f is None:
                print(f"FILE_READ @{off} 冇回覆", file=sys.stderr)
                return 1
            if f[0] == FILE_ERROR_RSP:
                print(f"FILE_READ 失敗：{_err_name(f[1])}", file=sys.stderr)
                return 1
            if len(f[1]) < 6:
                print("FILE_READ 回覆太短", file=sys.stderr)
                return 1
            roff = struct.unpack_from("<I", f[1], 2)[0]
            part = f[1][6:]
            if roff != off:
                print(f"FILE_READ offset 唔對：{roff} != {off}", file=sys.stderr)
                return 1
            if not part:
                break
            data.extend(part)
            off += len(part)
            if len(part) < chunk:
                break
    with open(local, "wb") as fp:
        fp.write(data)
    print(f"OK 下載 {remote} → {local}（{len(data)} B）")
    return 0


def cmd_put(port: str, baud: int, local: str, remote: str, chunk: int = 2048) -> int:
    import serial
    data = open(local, "rb").read()
    sha = hashlib.sha256(data).digest()
    fid = random.randint(1, 0xFFFF)
    begin = struct.pack("<HIH", fid, len(data), chunk) + sha + _enc_str(remote)
    with serial.Serial(port, baud, timeout=0.2) as ser:
        time.sleep(0.6)
        ser.reset_input_buffer()
        r = Reader(ser)
        ser.write(pack(FILE_BEGIN, begin))
        f = _wait(r, {FILE_ACK, FILE_ERROR_RSP}, 8.0)
        if f is None:
            print("FILE_BEGIN 冇回覆（裝置 app 未起？）", file=sys.stderr)
            return 1
        if f[0] == FILE_ERROR_RSP:
            print(f"FILE_BEGIN 失敗：{_err_name(f[1])}", file=sys.stderr)
            return 1
        off = 0
        t0 = time.time()
        while off < len(data):
            part = data[off:off + chunk]
            ser.write(pack(FILE_CHUNK, struct.pack("<HI", fid, off) + part))
            f = _wait(r, {FILE_ACK, FILE_ERROR_RSP}, 8.0)
            if f is None:
                print(f"FILE_CHUNK @{off} 冇回覆", file=sys.stderr)
                return 1
            if f[0] == FILE_ERROR_RSP:
                print(f"FILE_CHUNK @{off} 失敗：{_err_name(f[1])}", file=sys.stderr)
                return 1
            ack_off = struct.unpack_from("<I", f[1], 2)[0]
            if ack_off != off:
                print(f"FILE_CHUNK ACK offset 唔對：{ack_off} != {off}", file=sys.stderr)
                return 1
            off += len(part)
        ser.write(pack(FILE_END, struct.pack("<H", fid)))
        f = _wait(r, {FILE_QUERY_RSP, FILE_ERROR_RSP}, 8.0)
        dt = time.time() - t0
        if f is None:
            print("FILE_END 冇回覆", file=sys.stderr)
            return 1
        if f[0] == FILE_ERROR_RSP:
            print(f"FILE_END 失敗：{_err_name(f[1])}", file=sys.stderr)
            return 1
        print(f"OK 上傳 {local} → {remote}（{len(data)} B，{dt:.1f}s，"
              f"{len(data) / max(dt, 0.001) / 1024:.1f} KiB/s）sha256={sha.hex()[:16]}…")
        return 0


def cmd_simple(port: str, baud: int, cmd: str, path: str) -> int:
    """confirm（0x2008）／undo（0x200A）：payload = str_u16(path)。"""
    import serial
    op = FILE_CONFIRM if cmd == "confirm" else FILE_UNDO
    with serial.Serial(port, baud, timeout=0.2) as ser:
        time.sleep(0.6)
        ser.reset_input_buffer()
        r = Reader(ser)
        ser.write(pack(op, _enc_str(path)))
        f = _wait(r, {FILE_QUERY_RSP, FILE_ERROR_RSP}, 5.0)
        if f is None:
            print(f"{cmd} 冇回覆", file=sys.stderr)
            return 1
        if f[0] == FILE_ERROR_RSP:
            print(f"{cmd} 失敗：{_err_name(f[1])}", file=sys.stderr)
            return 1
        print(f"OK {cmd} {path}")
        return 0


def cmd_delete(port: str, baud: int, remote: str) -> int:
    import serial
    with serial.Serial(port, baud, timeout=0.2) as ser:
        time.sleep(0.6)
        ser.reset_input_buffer()
        r = Reader(ser)
        ser.write(pack(FILE_DELETE, _enc_str(remote)))
        f = _wait(r, {FILE_QUERY_RSP, FILE_ERROR_RSP}, 5.0)
        if f is None:
            print("FILE_DELETE 冇回覆", file=sys.stderr)
            return 1
        if f[0] == FILE_ERROR_RSP:
            print(f"FILE_DELETE 失敗：{_err_name(f[1])}", file=sys.stderr)
            return 1
        print(f"OK 刪除 {remote}")
        return 0


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
    p = sub.add_parser("put", help="上傳檔案（FILE_BEGIN/CHUNK/END）")
    p.add_argument("local")
    p.add_argument("remote")
    p.add_argument("--chunk", type=int, default=2048)
    g = sub.add_parser("get", help="下載檔案（FILE_READ）")
    g.add_argument("remote")
    g.add_argument("local")
    g.add_argument("--chunk", type=int, default=4096)
    d = sub.add_parser("delete", help="刪檔（FILE_DELETE）")
    d.add_argument("path")
    c = sub.add_parser("confirm", help="確認新檔、刪 .bak（FILE_CONFIRM）")
    c.add_argument("path")
    u = sub.add_parser("undo", help="退回 .bak（FILE_UNDO）")
    u.add_argument("path")
    args = ap.parse_args()

    import serial

    if args.cmd == "put":
        return cmd_put(args.port, args.baud, args.local, args.remote, args.chunk)
    if args.cmd == "get":
        return cmd_get(args.port, args.baud, args.remote, args.local, args.chunk)
    if args.cmd == "delete":
        return cmd_delete(args.port, args.baud, args.path)
    if args.cmd in ("confirm", "undo"):
        return cmd_simple(args.port, args.baud, args.cmd, args.path)

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
