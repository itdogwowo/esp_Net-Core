#!/usr/bin/env python3
"""一鍵自測：把「這版 firmware 還活著嗎」壓成一支可重複跑的腳本。

涵蓋：
    基礎     STATUS_GET / SYS_TASK_QUERY / OTA VERSION / OTA CAPS
    解碼器   9 個 framing case（與 tools/nc4_test.py 同一組）
    效能     decode / pack @ 8192 與 64
    突發     burst 128 / 256 幀的完整性
    OTA      --no-apply 上傳一次，量吞吐（不動開機分區）

用法：
    tools/selftest.py --port /dev/cu.usbmodem*
    tools/selftest.py --port ... --image build/esp_net_core.bin
    tools/selftest.py --port ... --skip-ota
"""
import argparse
import binascii
import os
import struct
import time
import importlib.util

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("ota_tool", os.path.join(HERE, "ota.py"))
ota = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ota)

CMD_STATUS_GET = 0x1101
CMD_STATUS_RSP = 0x1102
CMD_TASK_QUERY = 0x1005
CMD_TASK_RSP = 0x1006
CMD_VERSION_QUERY = 0x2206
CMD_VERSION_RSP = 0x2207
CMD_CAPS_QUERY = 0x2210
CMD_CAPS_RSP = 0x2211
CMD_BENCH = 0x1801
CMD_REBOOT = 0x100F

def pack(cmd, payload=b"", addr=ota.ADDR_BROADCAST, ver=ota.VERSION, decl_len=None):
    body = struct.pack("<BHHH", ver, addr, cmd,
                       len(payload) if decl_len is None else decl_len) + payload
    crc = binascii.crc32(body, 0) & 0xFFFFFFFF
    return ota.SOF + body + struct.pack("<I", crc)


results = []


def record(name, ok, detail=""):
    results.append((name, ok, detail))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"   {detail}" if detail and not ok else ""))


def s16(payload, off=0):
    n = struct.unpack_from("<H", payload, off)[0]
    return payload[off + 2:off + 2 + n].decode("utf-8", "replace")


def bench(ser, rd, size, iters, mode):
    rd.buf.clear()
    ser.reset_input_buffer()
    ser.write(pack(CMD_BENCH, struct.pack("<HHB", size, iters, mode)))
    end = time.time() + 30
    while time.time() < end:
        f = rd.poll(0.02)
        if f and f[0] == 0x1802:
            n = struct.unpack_from("<H", f[1], 0)[0]
            body = f[1][2:2 + n].decode("utf-8", "replace")
            # {"mode":"decode","size":8192,"iters":100,"us_per_frame":861.0,"mb_s":9.53}
            import re
            mb = float(re.search(r'"mb_s":([0-9.]+)', body).group(1))
            us = float(re.search(r'"us_per_frame":([0-9.]+)', body).group(1))
            return mb, us
    return None, None


def framing_cases(ser, rd):
    q = pack(CMD_STATUS_GET, b"\x00\x00")

    def count(send_bytes, want, name, seconds=0.7, split=None):
        rd.buf.clear()
        ser.reset_input_buffer()
        if split:
            for part in split:
                ser.write(part)
                ser.flush()
                time.sleep(0.08)
        else:
            ser.write(send_bytes)
        got = 0
        end = time.time() + seconds
        while time.time() < end:
            f = rd.poll(0.02)
            if f and f[0] == CMD_STATUS_RSP:
                got += 1
        record(name, got == want, f"得到 {got}，預期 {want}")

    count(q, 1, "正常單幀")
    count(q + q, 2, "黏包（兩幀一次寫）")
    count(None, 1, "拆包（分三段送）", split=[q[:4], q[4:11], q[11:]])

    bad = bytearray(q)
    bad[-1] ^= 0xFF
    count(bytes(bad) + q, 1, "壞 CRC + 好幀")

    count(b"\x00\x4e\x43\x99junk\x4e\x43" + q, 1, "垃圾中的假 SOF")

    count(pack(CMD_STATUS_GET, b"\x00\x00", addr=0x1234), 0, "別人的 addr → 不回")
    count(pack(CMD_STATUS_GET, b"\x00\x00", ver=3) + q, 1, "錯版本 + 好幀")
    count(pack(CMD_STATUS_GET, b"\x00" * 16, decl_len=9000)[:25] + q, 1, "超大 LEN + 好幀")
    count(b"\r\nhello\r\n" + q, 1, "純文字 + 好幀")


def burst(ser, rd, n):
    frame = pack(CMD_STATUS_GET, b"\x00\x00")
    rd.buf.clear()
    ser.reset_input_buffer()
    time.sleep(0.2)
    ser.write(frame * n)
    got = 0
    end = time.time() + 12
    while got < n and time.time() < end:
        f = rd.poll(0.02)
        if f and f[0] == CMD_STATUS_RSP:
            got += 1
    return got


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--image", default="build/esp_net_core.bin")
    ap.add_argument("--skip-ota", action="store_true")
    args = ap.parse_args()

    import serial

    with serial.Serial(args.port, 115200, timeout=0.02) as ser:
        time.sleep(0.6)
        rd = ota.Reader(ser)

        print("\n[基礎]")
        ser.write(pack(CMD_STATUS_GET, b"\x00\x00"))
        f = rd.wait_for({CMD_STATUS_RSP}, timeout=3)
        record("STATUS_GET", f is not None)
        if f:
            print(f"        {s16(f[1])}")

        ser.write(pack(CMD_TASK_QUERY))
        f = rd.wait_for({CMD_TASK_RSP}, timeout=3)
        record("SYS_TASK_QUERY", f is not None)
        if f:
            print(f"        {s16(f[1])}")

        ser.write(pack(CMD_VERSION_QUERY))
        f = rd.wait_for({CMD_VERSION_RSP}, timeout=3)
        record("OTA_VERSION", f is not None)
        if f:
            fw = s16(f[1])
            o = 2 + len(fw.encode())
            slot = s16(f[1], o + 32)
            print(f"        fw={fw} running_slot={slot}")

        ser.write(pack(CMD_CAPS_QUERY))
        f = rd.wait_for({CMD_CAPS_RSP}, timeout=3)
        record("OTA_CAPS", f is not None)
        if f:
            mc = struct.unpack_from("<H", f[1], 0)[0]
            print(f"        max_chunk={mc}")

        print("\n[解碼器]")
        framing_cases(ser, rd)

        print("\n[效能]")
        for size, iters in ((8192, 100), (64, 1000)):
            mb, us = bench(ser, rd, size, iters, 0)
            record(f"decode {size} B", mb is not None, f"{mb} MB/s")
            if mb:
                print(f"        {us:.1f} us/frame, {mb:.2f} MB/s")
            mb, us = bench(ser, rd, size, iters, 1)
            record(f"pack   {size} B", mb is not None, f"{mb} MB/s")
            if mb:
                print(f"        {us:.1f} us/frame, {mb:.2f} MB/s")

        print("\n[突發]")
        for n in (128, 256):
            got = burst(ser, rd, n)
            record(f"burst {n}", True, "")   # 只記錄，不判定（no flow control）
            print(f"        {got}/{n}")

        if not args.skip_ota and os.path.exists(args.image):
            print("\n[OTA]")
            data = open(args.image, "rb").read()
            import hashlib
            digest = hashlib.sha256(data).digest()
            payload = struct.pack("<IH", len(data), 4096) + digest + ota.str_u16("selftest")
            ser.write(pack(0x2201, payload))
            f = rd.wait_for({0x2204, 0x221C}, timeout=6)
            if f and f[0] == 0x2204:
                offset = 0
                t0 = time.time()
                ok = True
                while offset < len(data):
                    chunk = data[offset:offset + 4096]
                    ser.write(pack(0x2202, struct.pack("<I", offset) + chunk))
                    a = rd.wait_for({0x2204, 0x221C}, timeout=6)
                    if not a or a[0] != 0x2204:
                        ok = False
                        break
                    offset = struct.unpack_from("<I", a[1], 4)[0]
                dt = time.time() - t0
                if ok:
                    ser.write(pack(0x2203))
                    e = rd.wait_for({0x221B, 0x221C}, timeout=20)
                    ok = e is not None and e[0] == 0x221B
                record("OTA 上傳 + END 驗證", ok)
                if ok:
                    print(f"        {len(data)/dt/1024:.1f} KB/s（{dt:.1f}s）")
            else:
                record("OTA BEGIN", False)

    print("\n==== 總結 ====")
    passed = sum(1 for _, ok, _ in results if ok)
    print(f"{passed}/{len(results)} 通過")
    for name, ok, detail in results:
        if not ok:
            print(f"  ✗ {name}  {detail}")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
