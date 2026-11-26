#!/usr/bin/env python3
"""NCB2 原型：JSON → 緊湊二進位（字典＋TLV＋varint＋CRC32），量實際大小。

用途：驗證「唔直接存 JSON、改存 compact blob」值唔值得，同定 header 格式。
呢個係 lab 原型（唔入 firmware）；firmware 版會用同一格式但按固定 schema 編碼。

格式（v2）：
  header  = "NCB2"(4) | ver u16 | flags u16 | strtab_len u32 | payload_len u32 | crc32 u32  (20 B)
  strtab  = n u16, 然後 n × (len u8 + utf8)          ← 所有 key 只存一次
  payload = 值樹：tag u8
             0 null
             1 false / 2 true
             3 int      → zigzag varint
             4 str      → varint len + utf8（或 u16 strtab index，tag 5 借用）
             5 arr      → varint count + 值樹×count
             6 obj      → varint count + (key_ref: tag 7 + varint strtab_index) + 值樹
             7 keyref   → varint strtab index（只出現喺 obj 內）
  crc32 = zlib.crc32(strtab + payload)

對比：JSON 有重複 key、引號、縮排、十進位文字；NCB2 每個 key 只存一次、
數字用 varint（0..127 = 1 B）、bool 1 B。
"""
from __future__ import annotations

import json
import struct
import sys
import zlib
from pathlib import Path

MAGIC = b"NCB2"
VER = 2


class Writer:
    def __init__(self) -> None:
        self.buf = bytearray()

    def u8(self, v: int) -> None:
        self.buf += struct.pack("<B", v & 0xFF)

    def u16(self, v: int) -> None:
        self.buf += struct.pack("<H", v & 0xFFFF)

    def u32(self, v: int) -> None:
        self.buf += struct.pack("<I", v & 0xFFFFFFFF)

    def varint(self, v: int) -> None:
        v = int(v)
        if v < 0:
            v = (-v << 1) - 1          # zigzag
        else:
            v = v << 1
        while v > 0x7F:
            self.buf.append((v & 0x7F) | 0x80)
            v >>= 7
        self.buf.append(v)

    def raw(self, b: bytes) -> None:
        self.buf += b


def encode(obj) -> bytes:
    # ① 收集所有 key（同一次出現只存一次）
    keys: list[str] = []

    def walk(v) -> None:
        if isinstance(v, dict):
            for k, x in v.items():
                if k not in keys:
                    keys.append(k)
                walk(x)
        elif isinstance(v, list):
            for x in v:
                walk(x)

    walk(obj)
    idx = {k: i for i, k in enumerate(keys)}

    strtab = Writer()
    strtab.u16(len(keys))
    for k in keys:
        b = k.encode("utf-8")
        if len(b) > 255:
            raise ValueError(f"key 太長：{k!r}")
        strtab.u8(len(b))
        strtab.raw(b)

    pay = Writer()

    def val(v) -> None:
        if v is None:
            pay.u8(0)
        elif v is False:
            pay.u8(1)
        elif v is True:
            pay.u8(2)
        elif isinstance(v, int):
            pay.u8(3)
            pay.varint(v)
        elif isinstance(v, float):
            pay.u8(4)
            pay.raw(struct.pack("<d", v))
        elif isinstance(v, str):
            b = v.encode("utf-8")
            pay.u8(5)
            pay.varint(len(b))
            pay.raw(b)
        elif isinstance(v, list):
            pay.u8(6)
            pay.varint(len(v))
            for x in v:
                val(x)
        elif isinstance(v, dict):
            pay.u8(7)
            pay.varint(len(v))
            for k, x in v.items():
                pay.u8(8)          # keyref
                pay.varint(idx[k])
                val(x)
        else:
            raise TypeError(type(v))

    val(obj)

    head = Writer()
    head.raw(MAGIC)
    head.u16(VER)
    head.u16(0)                    # flags
    head.u32(len(strtab.buf))
    head.u32(len(pay.buf))
    head.u32(zlib.crc32(strtab.buf + pay.buf) & 0xFFFFFFFF)
    return bytes(head.buf) + bytes(strtab.buf) + bytes(pay.buf)


def main() -> int:
    root = Path(".")
    targets = [
        "ports/project/S3/ESP32-S3-Matrix-LED-Textkit/data/config/hardware.json",
        "ports/project/S3/ESP32-S3-Matrix/data/config/driver.json",
        "ports/project/S3/ESP32-S3-Matrix-LED-Textkit/data/config/system.json",
        "ports/project/S3/ESP32-S3-Matrix-LED-Textkit/data/config/tasks.json",
        "ports/project/S3/ESP32-S3-Matrix-LED-Textkit/data/config/resources.json",
        "ports/project/S3/ESP32-S3-Matrix-LED-Textkit/data/pixel/registry.json",
        "ports/project/S3/ESP32-S3-Matrix-LED-Textkit/data/pixel/modes/rainbow.json",
    ]
    total_j = total_b = 0
    print(f"{'檔案':64} {'JSON':>8} {'NCB2':>8} {'省':>7}")
    for rel in targets:
        p = root / rel
        if not p.is_file():
            print(f"{rel:64} （缺）")
            continue
        raw = p.read_bytes()
        obj = json.loads(raw.decode("utf-8"))
        blob = encode(obj)
        pct = (1 - len(blob) / max(1, len(raw))) * 100
        total_j += len(raw)
        total_b += len(blob)
        print(f"{rel:64} {len(raw):8} {len(blob):8} {pct:6.1f}%")
    if total_j:
        print(f"{'合計':64} {total_j:8} {total_b:8} {(1-total_b/total_j)*100:6.1f}%")
    return 0


if __name__ == "__main__":
    sys.exit(main())
