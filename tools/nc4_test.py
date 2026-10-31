#!/usr/bin/env python3
"""NC4 解碼器一致性測試 —— 對著真的板子跑，確認 framing 行為與 mp 版相同。

每個 case 都是「送什麼 → 期待什麼」，涵蓋：
    正常幀 / 黏包 / 拆包 / 壞 CRC / 垃圾中的假 SOF / 別人的 addr / 超大 LEN / 錯版本

用法：
    tools/nc4_test.py --port /dev/cu.usbmodem*
"""
import argparse
import binascii
import struct
import time

SOF = b"NC"
VERSION = 4
ADDR_BROADCAST = 0xFFFF
CMD_STATUS_GET = 0x1101
CMD_STATUS_RSP = 0x1102


def pack(cmd, payload=b"", addr=ADDR_BROADCAST, ver=VERSION, decl_len=None):
    body = struct.pack("<BHHH", ver, addr, cmd, len(payload) if decl_len is None else decl_len)
    body += payload
    crc = binascii.crc32(body, 0) & 0xFFFFFFFF
    return SOF + body + struct.pack("<I", crc)


class Reader:
    def __init__(self, ser):
        self.ser = ser
        self.buf = bytearray()

    def frames(self, seconds=0.6):
        """收集一段時間內解出的所有幀。"""
        end = time.time() + seconds
        out = []
        while time.time() < end:
            chunk = self.ser.read(4096)
            if chunk:
                self.buf += chunk
            out.extend(self._drain())
        return out

    def clear(self, seconds=0.2):
        self.frames(seconds)

    def _drain(self):
        got = []
        while True:
            idx = self.buf.find(SOF)
            if idx < 0:
                self.buf.clear()
                return got
            if idx:
                del self.buf[:idx]
            if len(self.buf) < 9:
                return got
            if self.buf[2] != VERSION:
                del self.buf[:1]
                continue
            cmd, ln = struct.unpack_from("<HH", self.buf, 5)
            if ln > 8192:
                del self.buf[:1]
                continue
            total = 9 + ln + 4
            if len(self.buf) < total:
                return got
            body = bytes(self.buf[2:9 + ln])
            crc = struct.unpack_from("<I", self.buf, 9 + ln)[0]
            if (binascii.crc32(body, 0) & 0xFFFFFFFF) != crc:
                del self.buf[:1]
                continue
            got.append((cmd, bytes(self.buf[9:9 + ln])))
            del self.buf[:total]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--cmd", type=lambda v: int(v, 0), default=CMD_STATUS_GET)
    ap.add_argument("--rsp", type=lambda v: int(v, 0), default=CMD_STATUS_RSP)
    args = ap.parse_args()

    import serial

    total = 0
    passed = 0

    def check(name, cond, detail=""):
        nonlocal total, passed
        total += 1
        if cond:
            passed += 1
            print(f"  PASS  {name}")
        else:
            print(f"  FAIL  {name}   {detail}")

    with serial.Serial(args.port, 115200, timeout=0.05) as ser:
        time.sleep(0.5)
        rd = Reader(ser)
        q = pack(args.cmd, b"\x00\x00")       # STATUS_GET(keys="")
        rsp = lambda fs: [f for f in fs if f[0] == args.rsp]

        print("1. 正常單幀")
        rd.clear()
        ser.write(q)
        check("收到 STATUS_RSP", len(rsp(rd.frames())) == 1)

        print("2. 黏包：兩個幀塞進一次 write")
        rd.clear()
        ser.write(q + q)
        check("兩幀都被解出", len(rsp(rd.frames())) == 2)

        print("3. 拆包：一個幀分三段送，中間有延遲")
        rd.clear()
        for part in (q[:4], q[4:11], q[11:]):
            ser.write(part)
            ser.flush()
            time.sleep(0.08)
        check("跨 feed 拼回一幀", len(rsp(rd.frames())) == 1)

        print("4. 壞 CRC 幀 + 正常幀")
        rd.clear()
        bad = bytearray(q)
        bad[-1] ^= 0xFF
        ser.write(bytes(bad) + q)
        fs = rsp(rd.frames())
        check("壞幀被丟、好幀被解（重同步）", len(fs) == 1, f"得到 {len(fs)} 幀")

        print("5. 垃圾中含假 SOF（'NC'）+ 正常幀")
        rd.clear()
        ser.write(b"\x00\x4e\x43\x99garbage\x4e\x43" + q)
        fs = rsp(rd.frames())
        check("從垃圾裡重同步成功", len(fs) == 1, f"得到 {len(fs)} 幀")

        print("6. 別人的 addr（0x1234）")
        rd.clear()
        ser.write(pack(args.cmd, b"\x00\x00", addr=0x1234))
        fs = rsp(rd.frames())
        check("非廣播且非本機 → 不回", len(fs) == 0, f"得到 {len(fs)} 幀")

        print("7. 錯版本（VER=3）")
        rd.clear()
        ser.write(pack(args.cmd, b"\x00\x00", ver=3) + q)
        fs = rsp(rd.frames())
        check("錯版本被丟、後面的好幀仍解出", len(fs) == 1, f"得到 {len(fs)} 幀")

        print("8. 宣告 LEN 超過上限（9000）+ 正常幀")
        rd.clear()
        ser.write(pack(args.cmd, b"\x00" * 16, decl_len=9000)[:25] + q)
        fs = rsp(rd.frames())
        check("超大 LEN 被丟、不影響後續", len(fs) == 1, f"得到 {len(fs)} 幀")

        print("9. 空 write 與純文字雜訊")
        rd.clear()
        ser.write(b"\r\nhello\r\n")
        ser.write(q)
        fs = rsp(rd.frames())
        check("純文字後仍能解出", len(fs) == 1, f"得到 {len(fs)} 幀")

    print(f"\n{passed}/{total} 通過")
    return 0 if passed == total else 1


if __name__ == "__main__":
    raise SystemExit(main())
