#!/usr/bin/env python3
"""看裝置的 log 管道（log 與 NC4 幀共用同一條 CDC）。

    tools/nc4_log.py --port /dev/cu.usbmodem*
    tools/nc4_log.py --port ... --status-every 2      # 順便每 2 秒問一次 STATUS_GET

為什麼要自己寫一個（唔用 nc4.py listen）：
  同一條串流現在有兩種東西 —— NC4 幀（二進位）同 log 行（\x1e 開頭的文字）。
  這支就是「電腦端解碼」的參考實作：
    · 見到 \x1e → 讀到 \n 為止，當一行文字
    · 見到 "NC" → 驗 VER、長度、**CRC**；成功才當幀
    · 任何失敗 → 只前進 1 byte（唔可以用 LEN 跳，否則假幀會吞掉真幀）
"""
import argparse
import binascii
import json
import struct
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("需要 pyserial：pip install pyserial")

import os

# 協議實作只有一份：tools/nc4_demux.py
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nc4_demux import Demux, decode_str_u16, pack  # noqa: E402

CMD_STATUS_GET = 0x1101
CMD_STATUS_RSP = 0x1102


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--status-every", type=float, default=0.0)
    ap.add_argument("--status-keys", default="log")
    ap.add_argument("--seconds", type=float, default=0.0, help="0 = 一直跑")
    args = ap.parse_args()

    dm = Demux()
    t_end = time.time() + args.seconds if args.seconds > 0 else None
    next_status = time.time() + 1.0

    def open_port(timeout_s=30.0):
        end = time.time() + timeout_s
        while time.time() < end:
            try:
                return serial.Serial(args.port, 115200, timeout=0.05)
            except Exception:
                time.sleep(0.1)
        return None

    ser = open_port()
    if ser is None:
        sys.exit("打唔開序列埠")

    while True:
        if t_end and time.time() >= t_end:
            break

        # 重開機時 port 會消失再出現：斷線就重連，不要當成錯誤
        if ser is None:
            ser = open_port()
            continue

        if args.status_every > 0 and time.time() >= next_status:
            try:
                ser.write(pack(CMD_STATUS_GET,
                               struct.pack("<H", len(args.status_keys)) +
                               args.status_keys.encode()))
                next_status = time.time() + args.status_every
            except serial.SerialException:
                pass

        try:
            data = ser.read(4096)
        except serial.SerialException:
            try:
                ser.close()
            except Exception:
                pass
            ser = None
            continue

        if data:
            for kind, v in dm.feed(data):
                if kind == "line":
                    print(f"[{time.strftime('%H:%M:%S')}] {v}", flush=True)
                else:
                    cmd, payload = v
                    if cmd == CMD_STATUS_RSP:
                        body = decode_str_u16(payload)
                        if body:
                            try:
                                j = json.loads(body)
                                print(f"  << STATUS {json.dumps(j, separators=(',', ':'))}",
                                      flush=True)
                            except Exception:
                                print(f"  << STATUS (JSON 壞) {body[:80]}", flush=True)
                    else:
                        print(f"  << frame 0x{cmd:04X} {len(payload)} B", flush=True)

    if ser is not None:
        ser.close()
    print(f"\n統計：lines={dm.lines} frames={dm.frames} crc_err={dm.crc_err} junk={dm.junk}",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
