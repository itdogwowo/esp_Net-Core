#!/usr/bin/env python3
"""NC4 指令小工具 — 對 C 版裝置送指令（不需要 IDF 環境）

用法：
    tools/nc4.py --port /dev/cu.usbmodem* reboot
    tools/nc4.py --port /dev/cu.usbmodem* raw 0x100F 00000000
    tools/nc4.py --port /dev/cu.usbmodem* listen       # 只監看輸出

wire 格式與 mp_Net-Core 相同：
    "NC" | VER=4 | ADDR(2 LE) | CMD(2 LE) | LEN(2 LE) | DATA | CRC32(4 LE)
    CRC 範圍 = VER..DATA（不含 SOF、不含 CRC）
"""
import argparse
import binascii
import struct
import sys
import time

SOF = b"NC"
VERSION = 4
ADDR_BROADCAST = 0xFFFF

CMD_REBOOT = 0x100F


def pack(cmd: int, payload: bytes = b"", addr: int = ADDR_BROADCAST) -> bytes:
    body = struct.pack("<BHHH", VERSION, addr, cmd, len(payload)) + payload
    crc = binascii.crc32(body, 0) & 0xFFFFFFFF
    return SOF + body + struct.pack("<I", crc)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--addr", type=lambda v: int(v, 0), default=ADDR_BROADCAST)
    sub = ap.add_subparsers(dest="action", required=True)
    sub.add_parser("reboot")

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

        elif args.action == "raw":
            payload = bytes.fromhex(args.payload_hex) if args.payload_hex else b""
            frame = pack(args.cmd, payload, args.addr)
            s.write(frame)
            print(f"→ cmd 0x{args.cmd:04X} 已送出 {len(frame)} bytes（{frame.hex()}）")
            time.sleep(1.0)

        elif args.action == "listen":
            end = time.time() + args.seconds
            buf = b""
            while time.time() < end:
                buf += s.read(4096)
            sys.stdout.write(buf.decode("utf-8", "replace"))
            return 0

        # 順便印一下裝置回吐的 log
        out = s.read(4096)
        if out:
            sys.stdout.write(out.decode("utf-8", "replace"))

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
