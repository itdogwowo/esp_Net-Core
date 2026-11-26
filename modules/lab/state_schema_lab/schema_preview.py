#!/usr/bin/env python3
"""schema 字典儲存原型：JSON 路徑 → (key_id, value) 對，量實際大小。

概念（用家提出）：
  - 硬件有上限（SPI ≤2、I2C ≤2、UART ≤3、I80 ≤2、GPIO ≤48…），所以 key 係有限集合；
  - schema 一次過定義「路徑 → u16 id + 型別 + 範圍 + 預設」；
  - JSON 只做人改輸入（用路徑名）；機存只存 id + varint 值（冇 key 字串）；
  - 讀回時用 id 反查路徑（schema 表），唔使 parse JSON。

id 佈局（u16）：
  高 byte = 域      low byte = 欄位／索引
  0x00 sys    0x02 eth    0x03 sdio   0x04 pin    0x05 pwm
  0x06 uart   0x07 i80    0x08 spi    0x09 i2c    0x0A i2s
  0x0C devices（LED…）    0x0D tasks/resources

呢個 lab 只做 prototype：手寫 Matrix 用到的 schema 子集，計 blob 大小。
"""
from __future__ import annotations

import json
import struct
from pathlib import Path

# ── schema（路徑 → id, 型別, 範圍）─────────────────────────────
# 型別：u8/u16/u32/i8/i16/bool/enum/str
SCHEMA: list[tuple[str, int, str, int, int]] = [
    # sys：0x00xx
    ("system.version", 0x0001, "u16", 1, 65535),
    ("system.frame_interval_ms", 0x0002, "u16", 1, 1000),
    ("system.num_pixels", 0x0003, "u16", 0, 65535),
    ("system.log_level", 0x0004, "u8", 0, 6),
    # i80：0x07xx；索引 0/1，field 偏移 0x10
    ("hardware.i80.0.data", 0x0701, "blob", 0, 0),          # 特殊：list
    ("hardware.i80.0.wr", 0x0702, "i8", -1, 48),
    ("hardware.i80.0.dc", 0x0703, "i8", -1, 48),
    ("hardware.i80.0.cs", 0x0704, "i8", -1, 48),
    ("hardware.i80.0.freq", 0x0705, "u32", 100000, 80000000),
    # spi：0x08xx
    ("hardware.spi.0.id", 0x0801, "u8", 0, 3),
    ("hardware.spi.0.baudrate", 0x0802, "u32", 1, 80000000),
    ("hardware.spi.0.sck", 0x0803, "i8", -1, 48),
    ("hardware.spi.0.mosi", 0x0804, "i8", -1, 48),
    ("hardware.spi.0.miso", 0x0805, "i8", -1, 48),
    # devices／driver：0x0Cxx（LED）
    ("driver.led.enable", 0x0C01, "bool", 0, 1),
    ("driver.led.backend", 0x0C02, "enum", 0, 6),
    ("driver.led.bus", 0x0C03, "i8", -1, 2),
    ("driver.led.lanes", 0x0C04, "u8", 1, 16),
    ("driver.led.px", 0x0C05, "u16", 1, 65535),
    ("driver.led.format", 0x0C06, "enum_fmt", 0, 7),
    ("driver.led.pclk_hz", 0x0C07, "u32", 100000, 80000000),
    ("driver.led.bit_cells", 0x0C08, "u8", 0, 8),
    ("driver.led.t0h", 0x0C09, "u8", 0, 8),
    ("driver.led.t1h", 0x0C0A, "u8", 0, 8),
    ("driver.led.present", 0x0C0B, "bool", 0, 1),
]

ID = {p: i for p, i, *_ in SCHEMA}
FMT = ["ws2812", "ws2812w", "ws2815", "ws2801", "apa102"]
BACKEND = ["none", "i80", "spi", "rmt", "parlio", "direct", "mock"]

CFG = Path("ports/project/S3/ESP32-S3-Matrix-LED-Textkit/data/config")
DRV = Path("ports/project/S3/ESP32-S3-Matrix/data/config/driver.json")


def zz(v: int) -> bytes:
    v = (-v << 1) - 1 if v < 0 else v << 1
    out = bytearray()
    while v > 0x7F:
        out.append((v & 0x7F) | 0x80)
        v >>= 7
    out.append(v)
    return bytes(out)


class Blob:
    def __init__(self) -> None:
        self.items: dict[int, int] = {}

    def put(self, path: str, value) -> None:
        if path not in ID:
            return
        kid = ID[path]
        if isinstance(value, str):
            if kid == ID["driver.led.format"]:
                value = FMT.index(value) if value in FMT else 0
            elif kid == ID["driver.led.backend"]:
                value = BACKEND.index(value) if value in BACKEND else 0
            else:
                return
        elif isinstance(value, bool):
            value = 1 if value else 0
        self.items[kid] = int(value)

    def encode(self) -> bytes:
        # header: "NCS1" | ver u16 | count u16 | crc32 u32  → 12 B
        body = bytearray()
        for kid in sorted(self.items):
            body += zz(kid)
            body += zz(self.items[kid])
        import zlib
        crc = zlib.crc32(bytes(body)) & 0xFFFFFFFF
        return b"NCS1" + struct.pack("<HH", 1, len(self.items)) + struct.pack("<I", crc) + bytes(body)


def main() -> int:
    hw = json.loads((CFG / "hardware.json").read_text(encoding="utf-8"))
    drv = json.loads(DRV.read_text(encoding="utf-8"))
    b = Blob()

    # hardware：i80[0]（Matrix 稀疏單腳）
    i80 = (hw.get("I80") or {}).get("list") or [{}]
    e = i80[0]
    b.put("hardware.i80.0.wr", e.get("wr", -1) if e.get("wr") is not None else -1)
    b.put("hardware.i80.0.dc", e.get("dc", -1) if e.get("dc") is not None else -1)
    b.put("hardware.i80.0.cs", e.get("cs", -1) if e.get("cs") is not None else -1)
    b.put("hardware.i80.0.freq", e.get("freq", 80000000))
    # data list：當 1 個「lane 數」+ 最多 16 個 i8（呢個 prototype 先存 count）
    data = [x for x in (e.get("data") or []) if isinstance(x, int) and x >= 0]
    b.put("hardware.i80.0.data", len(data))
    # spi[0]
    spi = (hw.get("SPI") or {}).get("list") or [{}]
    s = spi[0]
    g = (s.get("GPIO") or {})
    b.put("hardware.spi.0.id", s.get("id", 1))
    b.put("hardware.spi.0.baudrate", s.get("baudrate", 80000000))
    for k in ("sck", "mosi", "miso"):
        v = g.get(k)
        b.put(f"hardware.spi.0.{k}", -1 if v is None else v)
    # driver LED
    led = drv.get("LED") or {}
    for k in ("enable", "backend", "lanes", "px", "format"):
        if k in led:
            b.put(f"driver.led.{k}", led[k])
    b.put("driver.led.present", 1 if led else 0)
    bus = led.get("bus") or {}
    if "index" in bus:
        b.put("driver.led.bus", bus["index"])
    for k in ("pclk_hz", "bit_cells", "t0h", "t1h"):
        if k in (led.get("timing") or {}):
            b.put(f"driver.led.{k}", led["timing"][k])

    blob = b.encode()
    hw_raw = (CFG / "hardware.json").read_bytes()
    drv_raw = DRV.read_bytes()
    print(f"hardware.json        {len(hw_raw):6} B")
    print(f"driver.json          {len(drv_raw):6} B")
    print(f"合計 JSON            {len(hw_raw)+len(drv_raw):6} B")
    print(f"NCS1 blob（id+值）    {len(blob):6} B   ← 省 {(1-len(blob)/(len(hw_raw)+len(drv_raw)))*100:.1f}%")
    print(f"schema 條目數        {len(SCHEMA)}（編譯成 C 表：id→型別/範圍/預設）")
    print(f"實際寫入 key 數      {len(b.items)}")
    print(f"blob hex: {blob.hex()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
