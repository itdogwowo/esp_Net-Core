#!/usr/bin/env python3
"""NC4 指令小工具 — 對 C 版裝置送指令（不需要 IDF 環境）

用法：
    tools/nc4.py --port /dev/cu.usbmodem* reboot
    tools/nc4.py --port /dev/cu.usbmodem* status            # STATUS_GET 全部
    tools/nc4.py --port /dev/cu.usbmodem* status tasks      # 只看任務表
    tools/nc4.py --port /dev/cu.usbmodem* raw 0x100F 00000000
    tools/nc4.py --port /dev/cu.usbmodem* listen       # 只監看輸出

wire 格式與 mp_Net-Core 相同：
    "NC" | VER=4 | ADDR(2 LE) | CMD(2 LE) | LEN(2 LE) | DATA | CRC32(4 LE)
    CRC 範圍 = VER..DATA（不含 SOF、不含 CRC）
"""
import argparse
import binascii
import json
import struct
import sys
import time

import os

# 協議實作只有一份：tools/nc4_demux.py（嚴格 CRC、失敗只前進 1 byte）
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nc4_demux import ADDR_BROADCAST, SOF, VERSION, pack, Reader, Demux  # noqa: E402

CMD_REBOOT = 0x100F
CMD_STATUS_GET = 0x1101
CMD_STATUS_RSP = 0x1102


def wait_for(ser, want_cmd: int, seconds: float):
    """等一個指定的回覆幀；回傳 payload 或 None。"""
    f = Reader(ser).wait_for({want_cmd}, seconds)
    return None if f is None else f[1]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--addr", type=lambda v: int(v, 0), default=ADDR_BROADCAST)
    sub = ap.add_subparsers(dest="action", required=True)
    sub.add_parser("reboot")

    st = sub.add_parser("status", help="STATUS_GET（keys 空白 = 全部，? = 目錄）")
    st.add_argument("keys", nargs="?", default="")

    raw = sub.add_parser("raw")
    raw.add_argument("cmd", type=lambda v: int(v, 0))
    raw.add_argument("payload_hex", nargs="?", default="")
    listen = sub.add_parser("listen")
    listen.add_argument("--seconds", type=float, default=5)

    args = ap.parse_args()

    import serial  # pyserial

    with serial.Serial(args.port, args.baud, timeout=0.2) as s:
        # 等 DTR 建立（TinyUSB CDC 需要一點時間才會收資料）
        time.sleep(0.6)

        if args.action == "reboot":
            frame = pack(CMD_REBOOT, struct.pack("<I", 0), args.addr)
            s.write(frame)
            print(f"→ REBOOT (0x100F) 已送出 {len(frame)} bytes")
            time.sleep(1.0)

        elif args.action == "status":
            kb = args.keys.encode()
            payload = struct.pack("<H", len(kb)) + kb      # str_u16len
            s.write(pack(CMD_STATUS_GET, payload, args.addr))
            resp = wait_for(s, CMD_STATUS_RSP, 3.0)
            if resp is None:
                print("沒有收到 STATUS_RSP（裝置沒開？還是這條通道不支援回覆）",
                      file=sys.stderr)
                return 1
            n = struct.unpack_from("<H", resp)[0]
            text = resp[2:2 + n].decode("utf-8", "replace")
            try:
                print(json.dumps(json.loads(text), indent=2, ensure_ascii=False))
            except json.JSONDecodeError:
                print(text)
            return 0

        elif args.action == "raw":
            payload = bytes.fromhex(args.payload_hex) if args.payload_hex else b""
            frame = pack(args.cmd, payload, args.addr)
            s.write(frame)
            print(f"→ cmd 0x{args.cmd:04X} 已送出 {len(frame)} bytes（{frame.hex()}）")
            time.sleep(1.0)

        elif args.action == "listen":
            # log 行同 NC4 幀會交錯：用共用 Demux 分清，唔會把文字當幀
            dm = Demux()
            end = time.time() + args.seconds
            while time.time() < end:
                data = s.read(4096)
                if not data:
                    continue
                for kind, v in dm.feed(data):
                    if kind == "line":
                        print(v)
                    else:
                        cmd, payload = v
                        print(f"<< frame 0x{cmd:04X}  {len(payload)} B  {payload[:48].hex()}")
            return 0

        # 順便印一下裝置回吐的 log（reboot 後埠可能已經消失）
        try:
            out = s.read(4096)
            if out:
                sys.stdout.write(out.decode("utf-8", "replace"))
        except (serial.SerialException, OSError):
            pass

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

