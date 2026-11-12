#!/usr/bin/env python3
"""NC4 wire 協議（ESP 專用，獨立實作）。

wire 格式：
    "NC" | VER=4 | ADDR(2 LE) | CMD(2 LE) | LEN(2 LE) | DATA | CRC32(4 LE)
    CRC 範圍 = VER..DATA（不含 SOF、不含 CRC）

同一條 USB CDC 上仲有 log 行：RS(0x1E) 開頭、到 '\\n' 為止的 UTF-8 文字。
兩者交錯，所以分幀器一定要嚴格：
  1. 見到 "NC" 之後驗 VER、LEN、**CRC**，全對才算一幀。
  2. 任何失敗 → **只前進 1 byte**（唔可以用 LEN 跳，否則假幀會吞真幀）。
  3. RS 開頭到 '\\n' 係一行 log。

這份 codec 同 firmware 的 `nc4_pack_into` / `nc4_parser` 對齊；
唔依賴 mp_Net-Core 任何檔案。
"""
from __future__ import annotations

import binascii
import struct

SOF = b"NC"
VERSION = 4
ADDR_BROADCAST = 0xFFFF
MAX_PAYLOAD = 8192
HDR_LEN = 9
CRC_LEN = 4
RS = 0x1E


def pack(cmd: int, payload: bytes = b"", addr: int = ADDR_BROADCAST) -> bytes:
    """組一幀。cmd/payload 的長度由 caller 保證合法。"""
    body = struct.pack("<BHHH", VERSION, addr, cmd, len(payload)) + payload
    crc = binascii.crc32(body, 0) & 0xFFFFFFFF
    return SOF + body + struct.pack("<I", crc)


def str_u16(s: str, limit: int = 200) -> bytes:
    """str_u16len：2B 長度 + UTF-8。"""
    b = s.encode("utf-8")[:limit]
    return struct.pack("<H", len(b)) + b


def decode_str_u16(payload: bytes):
    """讀 str_u16len；長度不合理回 None。"""
    if len(payload) < 2:
        return None
    n = struct.unpack_from("<H", payload, 0)[0]
    if 2 + n > len(payload):
        return None
    return payload[2:2 + n].decode("utf-8", "replace")


class Demux:
    """純緩衝分幀器：feed(bytes) → [("line", text) | ("frame", (cmd, payload))]。

    唔綁任何 I/O，方便測試同 serial thread 共用。
    """

    def __init__(self) -> None:
        self.buf = b""
        self.lines = 0
        self.frames = 0
        self.crc_err = 0
        self.junk = 0

    def feed(self, data: bytes):
        self.buf += data
        out = []
        while self.buf:
            if self.buf[0] == RS:
                i = self.buf.find(b"\n")
                if i < 0:
                    break                      # 行未完整：等更多
                text = self.buf[1:i].decode("utf-8", "replace")
                self.lines += 1
                out.append(("line", text))
                self.buf = self.buf[i + 1:]
                continue

            if self.buf[:2] == SOF:
                if len(self.buf) < HDR_LEN:
                    break
                ver = self.buf[2]
                _addr, cmd, plen = struct.unpack_from("<HHH", self.buf, 3)
                end = HDR_LEN + plen + CRC_LEN
                if ver != VERSION or plen > MAX_PAYLOAD:
                    self.buf = self.buf[1:]    # 假幀：前進 1 byte
                    self.junk += 1
                    continue
                if len(self.buf) < end:
                    break
                crc = struct.unpack_from("<I", self.buf, HDR_LEN + plen)[0]
                if crc == (binascii.crc32(self.buf[2:HDR_LEN + plen]) & 0xFFFFFFFF):
                    out.append(("frame", (cmd, self.buf[HDR_LEN:HDR_LEN + plen])))
                    self.frames += 1
                    self.buf = self.buf[end:]
                else:
                    self.crc_err += 1
                    self.buf = self.buf[1:]    # CRC 錯：同樣前進 1 byte
                continue

            # 既非行標記、又非 SOF：跳到下一個可能的起點
            i = self.buf.find(bytes([RS]))
            j = self.buf.find(SOF)
            cand = [x for x in (i, j) if x >= 0]
            if cand:
                k = max(1, min(cand))
            else:
                # 全部垃圾。但尾端可能係「被 read 切開」的 SOF 前半（'N'）：
                # 留住佢，否則下一批帶住 'C…' 入嚟就永遠接唔返。
                k = len(self.buf) - 1 if self.buf.endswith(b"N") else len(self.buf)
            if k <= 0:
                break
            self.junk += k
            self.buf = self.buf[k:]
        return out
