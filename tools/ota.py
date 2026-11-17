#!/usr/bin/env python3
"""把 firmware 透過 NC4 OTA（0x22xx）推上板子 —— 不需要 download mode、不需要按鍵。

用法：
    tools/ota.py --port /dev/cu.usbmodem* firmware/build/esp_net_core.bin
    tools/ota.py --port /dev/cu.usbmodem* firmware/build/esp_net_core.bin --no-apply   # 只驗證不切換
    tools/ota.py --port /dev/cu.usbmodem* --abort          # 清掉殘留的 OTA session（唔寫任何嘢）

流程照 slave/schema/ota.json：
    0x2201 OTA_BEGIN  image_size(u32) chunk_size(u16) sha256(32B) fw_ver(str)
    0x2202 OTA_WRITE  offset(u32) data(bytes_rest)
    0x2204 OTA_ACK    offset(u32) written(u32)
    0x2203 OTA_END
    0x221B OTA_STATE_RSP  idle/writing/verified/error + target_slot
    0x221C OTA_ERROR_RSP  11 個 bool + failed_offset + written_up_to + target_slot
    0x2220 OTA_APPLY  set_boot_only(u8) restart_delay_ms(u32)

console 的 log 跟 NC4 回覆共用同一條 CDC，所以這裡自己掃 SOF + 驗 CRC 找回帧。

**BEGIN 要好等**（2026-11-17 修正）：韌體的 on_begin 會先 esp_ota_begin() 把目標
分區整段抹掉才回 ACK。1,224,272 B 的 test 映像在 S3 上實測 **5.17 s**（≈237 KB/s，
4 KB sector 逐個 erase）——舊版這裡硬等 5.0 s，於是映像大少少就變成假故障
（`BEGIN 失敗: no reply`，但裝置其實已經入咗 writing）。現在照映像大小估。
"""
import argparse
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
CMD_ABORT = 0x2205
CMD_VERSION_RSP = 0x2207
CMD_CAPS_RSP = 0x2211
CMD_PROGRESS_RSP = 0x2219
CMD_STATE_QUERY = 0x221A
CMD_STATE_RSP = 0x221B
CMD_ERROR_RSP = 0x221C
CMD_APPLY = 0x2220

ERROR_NAMES = [
    "err_begin_partition_conflict", "err_begin_no_partition", "err_begin_size_too_big",
    "err_write_offset_mismatch", "err_write_flash_fail", "err_write_chunk_too_big",
    "err_end_validate_fail", "err_end_not_writing", "err_apply_set_boot_fail",
    "err_not_in_verified_state", "err_already_active",
]

# 抹 flash 的實測速率 ≈ 237 KB/s（S3、16 MB QIO、4 KB sector）。用 100 KB/s 估
# （慢一倍的餘裕）再加 10 s 固定開銷：1.2 MB → 22 s；4 MB 上限 → 50 s。
_ERASE_BYTES_PER_S = 100_000.0


def begin_timeout_for(image_size: int) -> float:
    return 10.0 + image_size / _ERASE_BYTES_PER_S


def _str16(payload: bytes, off: int) -> str:
    if len(payload) < off + 2:
        return ""
    n = struct.unpack_from("<H", payload, off)[0]
    return payload[off + 2:off + 2 + n].decode("utf-8", "replace")


def query_state(ser, rd):
    """回 {'idle','writing','verified','target'} 或 None。"""
    ser.write(pack(CMD_STATE_QUERY))
    f = rd.wait_for({CMD_STATE_RSP}, 3.0)
    if f is None:
        return None
    p = f[1]
    return {
        "idle": bool(p[0]) if len(p) > 0 else False,
        "writing": bool(p[1]) if len(p) > 1 else False,
        "verified": bool(p[2]) if len(p) > 2 else False,
        "target": _str16(p, 4),
    }


def abort_session(ser, rd):
    """送 OTA_ABORT；成功回 True（state 回到 idle）。"""
    ser.write(pack(CMD_ABORT))
    return rd.wait_for({CMD_STATE_RSP}, 3.0) is not None


def begin(ser, rd, data: bytes, digest: bytes, chunk: int, fw_ver: str):
    """送 OTA_BEGIN，等 ACK／ERROR／超時。

    回傳 (ok, note)：ok=False 時 note 是給人看的失敗原因。
    """
    timeout = begin_timeout_for(len(data))
    payload = struct.pack("<IH", len(data), chunk) + digest + str_u16(fw_ver)
    print(f"BEGIN : 目標分區要先抹 {len(data) / 1024:.0f} KB"
          f"（估 ≤ {timeout:.0f} s），等 ACK…")
    t0 = time.time()
    ser.write(pack(CMD_BEGIN, payload))
    f = rd.wait_for({CMD_ACK, CMD_ERROR_RSP}, timeout=timeout)
    dt = time.time() - t0

    if f is None:
        # 回覆可能只是遲到（erase 比估算更慢）：用 STATE 問返實際情況。
        st = query_state(ser, rd)
        if st is not None and (st["writing"] or st["verified"]):
            print(f"BEGIN : ACK 未收到，但裝置已在 {st['target']} writing"
                  f"（等了 {dt:.1f} s）→ 當作已開始，繼續")
            return True, "late"
        return False, f"no reply（等咗 {timeout:.0f}s；state={st}）"

    if f[0] == CMD_ACK:
        offset, written = struct.unpack_from("<II", f[1])
        print(f"BEGIN ok（{dt:.1f} s，offset={offset} written={written}）")
        return True, "ok"

    flags = f[1][:len(ERROR_NAMES)]
    which = [ERROR_NAMES[i] for i, v in enumerate(flags) if v]
    print(f"BEGIN 失敗（{dt:.1f} s）:", ", ".join(which) if which else f[1].hex())
    if flags and flags[0]:      # err_begin_partition_conflict
        print("提示：裝置仍有 OTA session（多半是上次 BEGIN 超時留下的）。"
              "\n      tools/ota.py --port <port> --abort  清掉後再試。")
    return False, "error"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("image", nargs="?", help="要推的 app image（--abort 時可省略）")
    ap.add_argument("--chunk", type=int, default=4096)
    ap.add_argument("--no-apply", action="store_true")
    ap.add_argument("--delay-ms", type=int, default=500)
    ap.add_argument("--abort", action="store_true",
                    help="只送 OTA_ABORT 清掉殘留 session（不讀 image、不寫 flash）")
    args = ap.parse_args()

    if not args.abort and not args.image:
        ap.error("需要 image（或者用 --abort）")

    data = b""
    digest = b""
    fw_ver = ""
    if not args.abort:
        data = open(args.image, "rb").read()
        digest = hashlib.sha256(data).digest()
        fw_ver = "ota-" + time.strftime("%Y%m%d-%H%M%S")
        print(f"image : {args.image}  {len(data)} bytes")
        print(f"sha256: {digest.hex()}")

    import serial

    with serial.Serial(args.port, 115200, timeout=0.01) as ser:
        time.sleep(0.5)
        rd = Reader(ser)

        if args.abort:
            st = query_state(ser, rd)
            print(f"state : {st}")
            if st is not None and st["idle"]:
                print("已經係 idle，唔需要 ABORT")
                return 0
            if abort_session(ser, rd):
                print("ABORT ok → idle")
                return 0
            print("ABORT 失敗: no reply")
            return 1

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

        # ── 開始前先看狀態：上一次超時留下的 session 會令 BEGIN 回 conflict ──
        st = query_state(ser, rd)
        if st is not None and st["writing"]:
            print(f"state : 裝置仍在 writing（{st['target']}）→ 上次 OTA 未完成。"
                  "\n        先跑 `tools/ota.py --port <port> --abort` 再推。")
            return 1
        if st is not None:
            print(f"state : idle={int(st['idle'])} verified={int(st['verified'])} "
                  f"target={st['target'] or '-'}")

        # ── BEGIN ──
        ok, note = begin(ser, rd, data, digest, args.chunk, fw_ver)
        if not ok:
            print("BEGIN 失敗:", note)
            return 1

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
        f = rd.wait_for({CMD_STATE_RSP, CMD_ERROR_RSP}, timeout=30.0)
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
