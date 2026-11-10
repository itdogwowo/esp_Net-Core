#!/usr/bin/env python3
"""跑裝置端的統一緩衝自測（0xF032 BUF_TEST），可順便印 mem.buf。

用法：
    tools/buf_test.py --port COM8
    tools/buf_test.py --port COM8 --status      # 另外印 STATUS keys=mem 的 mem.buf

0xF032 做的事（見 firmware/components/action/test_action/nc_test_action.c）：
  ① 內部／PSRAM 兩條測試 buffer 的 64 B 對齊
  ② FIFO 次序 ＋ 滿了回 NULL
  ③ reserve（自己的表頭）＋ off/len
  ④ S2S / S2P / P2S / P2P 四條 copy 路徑（含 PSRAM 內容驗證）
  ⑤ 換手（記憶體位置不變、generation +1）

回覆是 0xF032：DATA = [u16 len][JSON]。
"""
import argparse
import json
import struct
import sys
import time

import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nc4_demux import ADDR_BROADCAST, pack, Reader  # noqa: E402

CMD_BUF_TEST = 0xF032
CMD_STATUS_GET = 0x1101
CMD_STATUS_RSP = 0x1102


def call(ser, cmd: int, payload: bytes, want: int, seconds: float = 6.0):
    ser.reset_input_buffer()
    ser.write(pack(cmd, payload, ADDR_BROADCAST))
    f = Reader(ser).wait_for({want}, seconds)
    if f is None:
        return None
    body = f[1]
    if len(body) < 2:
        return None
    n = struct.unpack_from("<H", body)[0]
    text = body[2:2 + n].decode("utf-8", "replace")
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        return {"_raw": text}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--status", action="store_true", help="另外印 mem.buf")
    ap.add_argument("--json", action="store_true", help="只印 JSON（不排版）")
    args = ap.parse_args()

    import serial

    with serial.Serial(args.port, args.baud, timeout=0.2) as s:
        time.sleep(0.6)

        got = call(s, CMD_BUF_TEST, b"", CMD_BUF_TEST)
        if got is None:
            print("沒有收到 0xF032 回覆（測試映像？NC_ENABLE_TESTS=ON 才有這條指令）",
                  file=sys.stderr)
            return 1

        if args.json:
            print(json.dumps(got, ensure_ascii=False))
        else:
            print(json.dumps(got, indent=2, ensure_ascii=False))

        ok = bool(got.get("ok"))
        if not ok:
            print("\nFAIL: " + json.dumps(
                {k: v for k, v in got.items() if k != "buf"}, ensure_ascii=False))
            return 1

        if args.status:
            st = call(s, CMD_STATUS_GET, struct.pack("<H", 3) + b"mem", CMD_STATUS_RSP)
            if st is None:
                print("沒有收到 STATUS_RSP", file=sys.stderr)
                return 1
            mem = st.get("mem", {})
            print("\nmem.buf = " + json.dumps(mem.get("buf", {}), indent=2,
                                              ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
