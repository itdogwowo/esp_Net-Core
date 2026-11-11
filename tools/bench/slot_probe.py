#!/usr/bin/env python3
"""一次性量測：用 0xF040 NETBENCH 對 CDC link 做 payload 掃描。

目的：驗證「4 格深度」在唔同 payload 下幾時開始掉幀 / 塞死。
唔改任何 firmware，只用已經燒喺板上的 0.1.2 測試版。

    python _tmp_slot_probe.py --port COM8
"""
import argparse
import binascii
import json
import struct
import sys
import time

import serial

SOF = b"NC"
ADDR = 0xFFFF
CMD_NETBENCH = 0xF040
CMD_RSP = 0xF004


def pack(cmd, payload=b"", addr=ADDR):
    body = struct.pack("<BHHH", 4, addr, cmd, len(payload)) + payload
    return SOF + body + struct.pack("<I", binascii.crc32(body) & 0xFFFFFFFF)


def drain(ser, want, timeout=2.0):
    """嚴格解析到指定 cmd 的回覆幀。"""
    buf = b""
    end = time.time() + timeout
    while time.time() < end:
        buf += ser.read(4096)
        i = buf.find(SOF)
        while i >= 0 and i + 9 <= len(buf):
            ver = buf[i + 2]
            _addr, cmd, ln = struct.unpack_from("<HHH", buf, i + 3)
            if ver != 4 or i + 9 + ln + 4 > len(buf):
                break
            body = buf[i + 2:i + 9 + ln]
            crc = struct.unpack_from("<I", buf, i + 9 + ln)[0]
            if (binascii.crc32(body) & 0xFFFFFFFF) == crc:
                data = buf[i + 9:i + 9 + ln]
                if cmd == want:
                    return data
                buf = buf[i + 9 + ln + 4:]
                i = buf.find(SOF)
                continue
            i = buf.find(SOF, i + 1)
        if i < 0:
            buf = buf[-8:] if len(buf) > 8 else buf
    return None


def net_row(ser):
    """STATUS_GET keys=net，回傳 dict（找不到就 None）。"""
    payload = struct.pack("<H", 3) + b"net"   # str_u16("net")
    ser.write(pack(0x1101, payload))
    data = drain(ser, 0x1102, 3.0)
    if data is None:
        return None
    try:
        return json.loads(data.decode("utf-8", "replace"))
    except Exception:
        return None


def as_json(data):
    """reply_json 前面帶 u16 長度，先去掉再 parse。"""
    if data is None:
        return {"err": "timeout"}
    if len(data) >= 2 and struct.unpack_from("<H", data, 0)[0] == len(data) - 2:
        data = data[2:]
    try:
        return json.loads(data.decode("utf-8", "replace"))
    except Exception:
        return {"err": "not_json", "raw": data[:60].hex()}


def run(ser, payload_len, count, gap_us=0):
    ser.reset_input_buffer()
    ser.write(pack(CMD_NETBENCH, b"\x00"))
    drain(ser, CMD_RSP, 1.5)

    body_tail = bytes(payload_len)
    t0 = time.time()
    for i in range(count):
        ser.write(pack(CMD_NETBENCH, b"\x03" + struct.pack("<I", i) + body_tail))
    t1 = time.time()
    ser.flush()

    ser.write(pack(CMD_NETBENCH, b"\x02"))
    data = drain(ser, CMD_RSP, 5.0)
    t2 = time.time()
    res = as_json(data)
    res["host_mbps"] = round(count * (payload_len + 13) / (t1 - t0) / 1e6, 2)
    res["wall_ms"] = round((t2 - t0) * 1000, 1)
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM8")
    ap.add_argument("--count", type=int, default=200)
    ap.add_argument("--sizes", default="32,256,1024,4096,8192")
    a = ap.parse_args()
    sizes = [int(x) for x in a.sizes.split(",")]

    with serial.Serial(a.port, 115200, timeout=0.2, write_timeout=5) as ser:
        print(f"# {a.port}  N={a.count}")
        print(f"{'payload':>8} {'frames':>7} {'lost':>5} {'missing':>8} "
              f"{'ooo':>4} {'fps':>8} {'kbps':>9} {'host MB/s':>10} {'wall ms':>8}")
        for p in sizes:
            r = run(ser, p, a.count)
            if "frames" in r:
                print(f"{p:>8} {r['frames']:>7} {r['lost']:>5} {r['missing']:>8} "
                      f"{r['ooo']:>4} {r['fps']:>8.1f} {r['kbps']:>9.1f} "
                      f"{r['host_mbps']:>10.2f} {r['wall_ms']:>8.1f}")
            else:
                print(f"{p:>8} {r}")
            time.sleep(0.3)

        row = net_row(ser)
        if row:
            for b in row.get("net", {}).get("bus", []):
                print(f"\n[{b['name']}] mode={b['mode']} slots={b['slots']} "
                      f"peak={b['peak']} slot_full={b['slot_full']} "
                      f"max_read={b['max_read']} frames={b['frames']} "
                      f"copied={b['copied']} region={b['region']}")


if __name__ == "__main__":
    sys.exit(main())
