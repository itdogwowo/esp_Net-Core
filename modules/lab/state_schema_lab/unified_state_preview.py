#!/usr/bin/env python3
"""統一 state 格式原型（NCSTATE）：內部自描述 record（type + key + len + value）。

用家定案：
  - 全部統一一個格式（state manager 唯一），**pixel 例外**（.layout 自己一個）；
  - **內部自描述**：每筆 record 帶 type／長度，reader 可以跳過唔識嘅；
  - magic 唔加版本數字（"NCSTATE" 冇 1），版本放 header ver u16。

Header（24 B）：
  magic 8B "NCSTATE"（NUL 結尾）
  ver u16 | flags u16 | count u16
  src_hash u32 | payload_len u32 | blob_crc u32

Record：
  type u8      1=unsigned int  2=signed int  3=bytes/str  4=bool  5=float?
  key varint   schema id（唔再存長路徑名）
  len varint   value 佔幾多 byte（未知 type 都可以 skip）
  value        (unsigned varint / zigzag varint / raw bytes)
"""
from __future__ import annotations

import json
import struct
import zlib
from pathlib import Path

MAGIC = b"NCSTATE" + b"\x00"
VER = 1

# type 代碼（自描述：reader 睇 type+len 就知點處理／跳過）
T_UINT, T_SINT, T_BYTES, T_BOOL = 1, 2, 3, 4


def varint(v: int) -> bytes:
    v = int(v)
    out = bytearray()
    while v > 0x7F:
        out.append((v & 0x7F) | 0x80)
        v >>= 7
    out.append(v)
    return bytes(out)


def zigzag(v: int) -> int:
    return (-v << 1) - 1 if v < 0 else v << 1


class Rec:
    def __init__(self) -> None:
        self.items: dict[int, tuple[int, bytes]] = {}

    def put_int(self, key: int, v: int) -> None:
        if v < 0:
            self.items[key] = (T_SINT, varint(zigzag(v)))
        else:
            self.items[key] = (T_UINT, varint(v))

    def put_bool(self, key: int, v: bool) -> None:
        self.items[key] = (T_BOOL, b"\x01" if v else b"\x00")

    def put_str(self, key: int, s: str) -> None:
        b = s.encode("utf-8")
        self.items[key] = (T_BYTES, varint(len(b)) + b)

    def encode(self) -> bytes:
        body = bytearray()
        for key in sorted(self.items):
            typ, val = self.items[key]
            body.append(typ)
            body += varint(key)
            body += varint(len(val))
            body += val
        head = (MAGIC + struct.pack("<HHH", VER, 0, len(self.items))
                + struct.pack("<III", 0, len(body), zlib.crc32(bytes(body)) & 0xFFFFFFFF))
        return head + bytes(body)


def main() -> int:
    cfg = Path("ports/project/S3/ESP32-S3-Matrix-LED-Textkit/data/config")
    drv = Path("ports/project/S3/ESP32-S3-Matrix/data/config/driver.json")
    hw_raw = (cfg / "hardware.json").read_bytes()
    drv_raw = drv.read_bytes()
    hw = json.loads(hw_raw)
    drvj = json.loads(drv_raw)

    r = Rec()
    i80 = (hw.get("I80") or {}).get("list") or [{}]
    e = i80[0]
    r.put_int(0x0701, len([x for x in (e.get("data") or []) if isinstance(x, int) and x >= 0]))
    for kid, name in ((0x0702, "wr"), (0x0703, "dc"), (0x0704, "cs")):
        v = e.get(name)
        r.put_int(kid, -1 if v is None else v)
    r.put_int(0x0705, e.get("freq", 80000000))
    led = drvj.get("LED") or {}
    r.put_bool(0x0C01, bool(led.get("enable")))
    for kid, name in ((0x0C04, "lanes"), (0x0C05, "px")):
        if name in led:
            r.put_int(kid, led[name])
    if led.get("format"):
        r.put_str(0x0C06, led["format"])
    blob = r.encode()

    print(f"JSON 合計            {len(hw_raw)+len(drv_raw):6} B")
    print(f"NCSTATE blob         {len(blob):6} B  → 省 {(1-len(blob)/(len(hw_raw)+len(drv_raw)))*100:.1f}%")
    print(f"header               {24:6} B（magic 8 + ver/flags/count 6 + src_hash/len/crc 12）")
    print(f"records              {len(blob)-24:6} B（{len(r.items)} 筆）")
    print(f"每筆平均             {(len(blob)-24)/max(1,len(r.items)):6.1f} B（type+key+len+value）")
    print()
    print("hexdump:")
    for i in range(0, len(blob), 16):
        chunk = blob[i:i+16]
        print(f"  {i:04x}  " + " ".join(f"{b:02x}" for b in chunk) + "  " +
              "".join(chr(b) if 32 <= b < 127 else "." for b in chunk))
    print()
    print("舊 NCS1（id+值，冇 type/len）估算：")
    old = 16 + sum(len(varint(k)) + len(v) for k, (_, v) in r.items.items())
    print(f"  約 {old} B；統一格式多 {len(blob)-old} B（換嚟自描述＋可跳過未知欄位）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
