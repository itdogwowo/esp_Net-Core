#!/usr/bin/env python3
"""量「裝置 → 主機」的 CDC 吞吐（0xF00D CDC_TX_BENCH）。

用途：log 管道要用幾大個 ring，取決於 CDC 實際推得幾快 —— 先量，再定。

    tools/bench/cdc_tx.py --port /dev/cu.usbmodem* 
    tools/bench/cdc_tx.py --port ... --bytes 262144 --chunk 64,256,1024 --mode 0,1

mode 0 = 連續寫、最後 flush 一次（理想吞吐）
mode 1 = 每個 chunk flush 一次（模擬「一行 flush 一次」）

注意：量測前會先清空輸入緩衝；裝置端在 `NC_ENABLE_TESTS=ON` 的 build 才有這條指令。
"""
import argparse
import binascii
import statistics
import struct
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("需要 pyserial：pip install pyserial")

SOF = b"NC"
VERSION = 4
ADDR_BROADCAST = 0xFFFF
MAX_PAYLOAD = 8192

CMD_CDC_TX_BENCH = 0xF00D
CMD_DEV_RSP = 0xF002


def pack(cmd: int, payload: bytes = b"", addr: int = ADDR_BROADCAST) -> bytes:
    body = struct.pack("<BHHH", VERSION, addr, cmd, len(payload)) + payload
    crc = binascii.crc32(body, 0) & 0xFFFFFFFF
    return SOF + body + struct.pack("<I", crc)


def parse_frames(buf: bytes):
    """嚴格解析：驗 VER、長度、CRC；失敗只前進 1 byte（唔可以用 LEN 跳）。

    回傳 (frames, 未用完的尾巴)。失敗前進 1 byte 是關鍵 ——
    這樣一串裸文字裡的假 "NC" 最多浪費 1 byte，不會吞掉後面真幀。
    """
    out = []
    i = 0
    while i + 9 <= len(buf):
        j = buf.find(SOF, i)
        if j < 0:
            i = len(buf)
            break
        if j + 9 > len(buf):
            i = j
            break
        ver = buf[j + 2]
        _addr, cmd, plen = struct.unpack_from("<HHH", buf, j + 3)
        end = j + 9 + plen + 4
        if ver == VERSION and plen <= MAX_PAYLOAD and end <= len(buf):
            crc = struct.unpack_from("<I", buf, j + 9 + plen)[0]
            if crc == (binascii.crc32(buf[j + 2:j + 9 + plen]) & 0xFFFFFFFF):
                out.append((cmd, buf[j + 9:j + 9 + plen]))
                i = end
                continue
        i = j + 1
    return out, buf[i:]


def wait_rsp(ser, want_cmd: int, seconds: float = 3.0):
    buf = b""
    end = time.time() + seconds
    while time.time() < end:
        chunk = ser.read(256)
        if chunk:
            buf += chunk
        frames, buf = parse_frames(buf)
        for cmd, payload in frames:
            if cmd == want_cmd:
                return payload
    return None


def run_once(ser, nbytes: int, chunk: int, mode: int):
    ser.reset_input_buffer()
    ser.write(pack(CMD_CDC_TX_BENCH, struct.pack("<IHB", nbytes, chunk, mode)))
    ser.flush()

    got = 0
    t0 = time.perf_counter()
    while got < nbytes:
        b = ser.read(min(4096, nbytes - got))
        if not b:
            break
        got += len(b)
    t1 = time.perf_counter()

    rsp = wait_rsp(ser, CMD_DEV_RSP)
    dev_us = None
    stalls = None
    if rsp is not None:
        # payload 是 str_u16len 編碼：u16 長度 + JSON
        if len(rsp) >= 2:
            n = struct.unpack_from("<H", rsp, 0)[0]
            body = rsp[2:2 + n].decode("utf-8", "replace")
            import json as _json
            try:
                j = _json.loads(body)
                dev_us = j.get("dev_us")
                stalls = j.get("stalls")
            except Exception:
                pass

    host_s = t1 - t0
    host_kbs = (got / host_s / 1024.0) if host_s > 0 else 0.0
    dev_kbs = None
    if dev_us:
        dev_kbs = (got / (dev_us / 1e6) / 1024.0)
    return got, host_s, host_kbs, dev_kbs, stalls


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--bytes", type=int, default=65536)
    ap.add_argument("--chunk", default="64,256,1024,4096")
    ap.add_argument("--mode", default="0,1")
    ap.add_argument("--repeat", type=int, default=3)
    args = ap.parse_args()

    chunks = [int(x) for x in args.chunk.split(",") if x]
    modes = [int(x) for x in args.mode.split(",") if x]

    print(f"{'chunk':>6} {'mode':>4} {'host KB/s':>11} {'dev KB/s':>10} {'stalls':>7} {' bytes':>8}")
    print("-" * 52)
    with serial.Serial(args.port, 115200, timeout=1.0) as ser:
        time.sleep(0.2)
        for mode in modes:
            for chunk in chunks:
                rates, devs, stalls_all = [], [], []
                for _ in range(args.repeat):
                    got, _, hk, dk, st = run_once(ser, args.bytes, chunk, mode)
                    if got != args.bytes:
                        print(f"{chunk:>6} {mode:>4}  收到 {got}/{args.bytes} bytes（中止）")
                        break
                    rates.append(hk)
                    devs.append(dk if dk else 0)
                    stalls_all.append(st if st is not None else -1)
                if rates:
                    print(f"{chunk:>6} {mode:>4} {statistics.median(rates):>11.1f} "
                          f"{statistics.median(devs):>10.1f} "
                          f"{max(stalls_all):>7} {args.bytes:>8}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
