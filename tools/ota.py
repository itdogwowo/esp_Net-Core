#!/usr/bin/env python3
"""把 firmware 透過 NC4 OTA（0x22xx）推上板子 —— 不需要 download mode、不需要按鍵。

用法：
    tools/ota.py --port /dev/cu.usbmodem* firmware/build/esp_net_core.bin
    tools/ota.py --port /dev/cu.usbmodem* firmware/build/esp_net_core.bin --no-apply   # 只驗證不切換

流程照 slave/schema/ota.json：
    0x2201 OTA_BEGIN  image_size(u32) chunk_size(u16) sha256(32B) fw_ver(str)
    0x2202 OTA_WRITE  offset(u32) data(bytes_rest)
    0x2204 OTA_ACK    offset(u32) written(u32)
    0x2203 OTA_END
    0x221B OTA_STATE_RSP  idle/writing/verified/error + target_slot
    0x221C OTA_ERROR_RSP  11 個 bool + failed_offset + written_up_to + target_slot
    0x2220 OTA_APPLY  set_boot_only(u8) restart_delay_ms(u32)

console 的 log 跟 NC4 回覆共用同一條 CDC，所以這裡自己掃 SOF + 驗 CRC 找回帧。
"""
import argparse
import binascii
import hashlib
import os
import struct
import sys
import time

# 協議實作只有一份：tools/nc4_demux.py（嚴格 CRC、失敗只前進 1 byte）
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nc4_demux import ADDR_BROADCAST, SOF, VERSION, pack, str_u16, Reader  # noqa: E402

CMD_BEGIN = 0x2201
CMD_WRITE = 0x2202
CMD_END = 0x2203
CMD_ACK = 0x2204
CMD_VERSION_RSP = 0x2207
CMD_CAPS_RSP = 0x2211
CMD_PROGRESS_RSP = 0x2219
CMD_STATE_RSP = 0x221B
CMD_ERROR_RSP = 0x221C
CMD_APPLY = 0x2220

ERROR_NAMES = [
    "err_begin_partition_conflict", "err_begin_no_partition", "err_begin_size_too_big",
    "err_write_offset_mismatch", "err_write_flash_fail", "err_write_chunk_too_big",
    "err_end_validate_fail", "err_end_not_writing", "err_apply_set_boot_fail",
    "err_not_in_verified_state", "err_already_active",
]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("image")
    ap.add_argument("--chunk", type=int, default=4096)
    ap.add_argument("--no-apply", action="store_true")
    ap.add_argument("--delay-ms", type=int, default=500)
    args = ap.parse_args()

    data = open(args.image, "rb").read()
    digest = hashlib.sha256(data).digest()
    fw_ver = "ota-" + time.strftime("%Y%m%d-%H%M%S")
    print(f"image : {args.image}  {len(data)} bytes")
    print(f"sha256: {digest.hex()}")

    import serial

    with serial.Serial(args.port, 115200, timeout=0.01) as ser:
        time.sleep(0.5)
        rd = Reader(ser)

        # ── 能力查詢 ──
        ser.write(pack(0x2210))
        f = rd.wait_for({CMD_CAPS_RSP}, timeout=2.0)
        if f:
            max_chunk, sb, fe, rb, diff = struct.unpack_from("<HBBBB", f[1])
            print(f"caps  : max_chunk={max_chunk} secure_boot={sb} rollback={rb}")
            if args.chunk > max_chunk:
                args.chunk = max_chunk
        else:
            print("caps  : 沒回覆（繼續，用預設 chunk）")

        # ── BEGIN ──
        payload = struct.pack("<IH", len(data), args.chunk) + digest + str_u16(fw_ver)
        ser.write(pack(CMD_BEGIN, payload))
        f = rd.wait_for({CMD_ACK, CMD_ERROR_RSP}, timeout=5.0)
        if f is None or f[0] == CMD_ERROR_RSP:
            print("BEGIN 失敗:", f[1].hex() if f else "no reply")
            return 1
        print("BEGIN ok")

        # ── WRITE ──
        offset = 0
        t0 = time.time()
        while offset < len(data):
            chunk = data[offset:offset + args.chunk]
            ser.write(pack(CMD_WRITE, struct.pack("<I", offset) + chunk))
            f = rd.wait_for({CMD_ACK, CMD_ERROR_RSP}, timeout=5.0)
            if f is None or f[0] == CMD_ERROR_RSP:
                print(f"\nWRITE 失敗 @ {offset}:", f[1].hex() if f else "no reply")
                return 1
            ack_off, written = struct.unpack_from("<II", f[1])
            if ack_off != offset:
                print(f"\noffset 回聲不符: {ack_off} != {offset}")
                return 1
            offset = written
            pct = 100.0 * offset / len(data)
            print(f"\r  {offset}/{len(data)}  {pct:5.1f}%", end="", flush=True)
        dt = time.time() - t0
        print(f"\nWRITE ok（{dt:.1f}s，{len(data)/dt/1024:.1f} KB/s）")

        # ── END ──
        ser.write(pack(CMD_END))
        f = rd.wait_for({CMD_STATE_RSP, CMD_ERROR_RSP}, timeout=15.0)
        if f is None or f[0] == CMD_ERROR_RSP:
            if f and f[0] == CMD_ERROR_RSP:
                flags = f[1][:11]
                which = [ERROR_NAMES[i] for i, v in enumerate(flags) if v]
                print("END 失敗:", which)
            else:
                print("END 失敗: no reply")
            return 1
        print("END ok（驗證通過）")

        if args.no_apply:
            print("--no-apply：不切換開機分區")
            return 0

        # ── APPLY ──
        ser.write(pack(CMD_APPLY, struct.pack("<BI", 0, args.delay_ms)))
        rd.wait_for({CMD_STATE_RSP}, timeout=3.0)
        print("APPLY 已送出，板子重啟中…")
        ser.close()

    time.sleep(args.delay_ms / 1000.0 + 2.0)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
