#!/usr/bin/env python3
"""NC4 wire 協議的**唯一**實作（所有 host 工具共用）。

為什麼要共用：log 管道之後，同一條 CDC 同時有兩種東西 ——
NC4 幀（二進位）同 log 行（\\x1e 開頭的文字）。任何一支工具自己寫 parser，
就會出現「有的跳過文字、有的唔跳」的不一致，而 log 文字真的會出現 "NC4"、
"NC-2B4C" 這種字串。

嚴格規則（三條，缺一不可）：
  1. 找到 "NC" 之後，驗 VER、LEN 合理、**CRC32 正確**，才算一幀。
  2. 任何失敗 → **只前進 1 byte**（唔可以用 LEN 跳；否則一個假幀會吞掉後面真幀）。
  3. 以 RS(0x1E) 開頭、到 '\\n' 為止是 log 行。

wire 格式：
    "NC" | VER=4 | ADDR(2 LE) | CMD(2 LE) | LEN(2 LE) | DATA | CRC32(4 LE)
    CRC 範圍 = VER..DATA（不含 SOF、不含 CRC）
"""
import binascii
import time
import struct

SOF = b"NC"
VERSION = 4
ADDR_BROADCAST = 0xFFFF
MAX_PAYLOAD = 8192
HDR_LEN = 9
CRC_LEN = 4
RS = 0x1E


def pack(cmd: int, payload: bytes = b"", addr: int = ADDR_BROADCAST) -> bytes:
    body = struct.pack("<BHHH", VERSION, addr, cmd, len(payload)) + payload
    crc = binascii.crc32(body, 0) & 0xFFFFFFFF
    return SOF + body + struct.pack("<I", crc)


def str_u16(s: str, limit: int = 200) -> bytes:
    b = s.encode()[:limit]
    return struct.pack("<H", len(b)) + b


def decode_str_u16(payload: bytes):
    if len(payload) < 2:
        return None
    n = struct.unpack_from("<H", payload, 0)[0]
    if 2 + n > len(payload):
        return None
    return payload[2:2 + n].decode("utf-8", "replace")


class Demux:
    """純緩衝分幀器：feed(bytes) → [('line', text) | ('frame', (cmd, payload))]。

    給「自己控制讀取」的工具用（例：同時印 log 同回覆）。
    """

    def __init__(self):
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
                # 全部都係垃圾。但尾端可能係「被讀取切開」的 SOF 前半（'N'）：
                # 留住佢，否則下一個 read 帶住 'C…' 入嚟就永遠接唔返，白白丟一幀。
                k = len(self.buf) - 1 if self.buf.endswith(b"N") else len(self.buf)
            if k <= 0:
                break                      # 只餘一個 'N'：等下一批 byte（唔可以原地打轉）
            self.junk += k
            self.buf = self.buf[k:]
        return out


def parse_frames(buf: bytes):
    """一次過解析一段緩衝。回 (frames, rest)；rest 是未完成的尾巴。

    給「讀一次、立刻要答案」的工具用（例：FILE_QUERY）。
    """
    d = Demux()
    events = d.feed(buf)
    frames = [(v[0], v[1]) for kind, v in events if kind == "frame"]
    return frames, d.buf


class Reader:
    """綁定 serial 的分幀器：poll()/wait_for()。

    跳過 log 行（佢哋唔會阻塞幀的解析），適合「只想要回覆」的工具。
    """

    def __init__(self, ser):
        self.ser = ser
        self.buf = bytearray()

    def poll(self, timeout: float = 0.01):
        end = __import__("time").time() + timeout
        while __import__("time").time() < end:
            # 只讀「已經到的」bytes，而且**唔可以**用阻塞式 read：
            #   · pyserial 的 read(N) 在不足 N bytes 時會等滿埠的 timeout 才返回
            #     → 每次量到的延遲都多了一個 timeout（實測 +10～50 ms）；
            #   · 用 read(1) 等資料又會在同一個 poll 內阻塞超過 timeout。
            # 所以：有資料就讀走，冇資料就睡 1 ms 再問（量測粒度 1 ms）。
            n = self.ser.in_waiting
            if n == 0:
                time.sleep(0.001)
                continue
            chunk = self.ser.read(n)
            if chunk:
                self.buf += chunk
            self._trim()
            frame = self._parse()
            if frame:
                return frame
        return None

    def wait_for(self, cmds, timeout: float = 5.0):
        end = __import__("time").time() + timeout
        while __import__("time").time() < end:
            f = self.poll(0.01)
            if f is None:
                continue
            if f[0] in cmds:
                return f
        return None

    def _trim(self):
        if len(self.buf) > 16384:
            del self.buf[:-8192]

    def _parse(self):
        while True:
            idx = self.buf.find(SOF)
            if idx < 0:
                self.buf.clear()
                return None
            if idx:
                del self.buf[:idx]
            if len(self.buf) < HDR_LEN:
                return None
            if self.buf[2] != VERSION:
                del self.buf[:1]               # 假幀：前進 1 byte
                continue
            cmd, ln = struct.unpack_from("<HH", self.buf, 5)
            if ln > MAX_PAYLOAD:
                del self.buf[:1]
                continue
            total = HDR_LEN + ln + CRC_LEN
            if len(self.buf) < total:
                return None
            body = bytes(self.buf[2:HDR_LEN + ln])
            crc = struct.unpack_from("<I", self.buf, HDR_LEN + ln)[0]
            if (binascii.crc32(body, 0) & 0xFFFFFFFF) != crc:
                del self.buf[:1]               # CRC 錯：前進 1 byte
                continue
            payload = bytes(self.buf[HDR_LEN:HDR_LEN + ln])
            del self.buf[:total]
            return cmd, payload
