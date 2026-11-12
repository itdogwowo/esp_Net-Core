#!/usr/bin/env python3
"""NC4 檔案操作 client（ESP 專用）。

流程同 mp_Net-Core 的 MPY 體驗一致，但用 ESP 的 ack 語義：
  upload:   BEGIN → [CHUNK → ACK]* → END → QUERY_RSP
  download: QUERY → [READ → CHUNK(file_id=0)]*
  list:     0x200B FILE_SCAN → 寫入 /manifest.json → FILE_READ manifest
  delete / confirm / undo: 一條指令 → QUERY_RSP

同 firmware 的唯一差別：BEGIN 成功會回 0x2004（offset=0），失敗回 0x2010。
mp 版係唔回 BEGIN 的；呢個工具只服務 ESP，所以用顯式 ACK，錯咗即刻知。
"""
from __future__ import annotations

import hashlib
import json
import struct
from typing import Callable

import nc4
from link import LinkError

CMD_FILE_BEGIN = 0x2001
CMD_FILE_CHUNK = 0x2002
CMD_FILE_END = 0x2003
CMD_FILE_ACK = 0x2004
CMD_FILE_QUERY = 0x2005
CMD_FILE_QUERY_RSP = 0x2006
CMD_FILE_READ = 0x2007
CMD_FILE_CONFIRM = 0x2008
CMD_FILE_DELETE = 0x2009
CMD_FILE_UNDO = 0x200A
CMD_FILE_SCAN = 0x200B
CMD_FILE_ERROR_RSP = 0x2010

ERROR_FIELDS = [
    "err_no_space", "err_write_fail", "err_offset_mismatch", "err_id_mismatch",
    "err_sha_mismatch", "err_not_active", "err_busy",
]
ERROR_TEXT = {
    "err_no_space": "裝置空間不足",
    "err_write_fail": "寫入失敗（路徑／FAT）",
    "err_offset_mismatch": "offset 對唔上（重試或由頭再上）",
    "err_id_mismatch": "file_id 對唔上（舊 session？）",
    "err_sha_mismatch": "SHA-256 校驗失敗",
    "err_not_active": "冇進行中的上傳 session",
    "err_busy": "FAT 唔係裝置手上（MSC 掛住）或已有 session",
}


class FileError(RuntimeError):
    def __init__(self, kind: str, message: str = "", detail: dict | None = None):
        self.kind = kind
        self.detail = detail or {}
        super().__init__(message or ERROR_TEXT.get(kind, kind))


def _decode_str16(payload: bytes) -> tuple[str, int]:
    if len(payload) < 2:
        return "", 0
    n = struct.unpack_from("<H", payload, 0)[0]
    if 2 + n > len(payload):
        return "", 0
    return payload[2:2 + n].decode("utf-8", "replace"), 2 + n


def _parse_query(payload: bytes) -> dict:
    if len(payload) < 1 + 32 + 4:
        raise FileError("bad_reply", "FILE_QUERY_RSP 太短")
    exists = payload[0]
    sha = payload[1:33]
    size = struct.unpack_from("<I", payload, 33)[0]
    path, used = _decode_str16(payload[37:])
    free = struct.unpack_from("<I", payload, 37 + used)[0] if len(payload) >= 37 + used + 4 else 0
    pending = payload[37 + used + 4] if len(payload) > 37 + used + 4 else 0
    return {"exists": bool(exists), "sha256": sha, "size": size, "path": path,
            "free": free, "pending": bool(pending)}


def parse_error(payload: bytes) -> FileError:
    flags = payload[:7]
    detail = {name: bool(flags[i]) for i, name in enumerate(ERROR_FIELDS)}
    failed, written = 0, 0
    if len(payload) >= 15:
        failed, written = struct.unpack_from("<II", payload, 7)
    path, _ = _decode_str16(payload[15:]) if len(payload) > 15 else ("", 0)
    detail.update(failed_offset=failed, written_up_to=written, path=path)
    kind = next((k for k, v in detail.items() if k in ERROR_FIELDS and v), "err_write_fail")
    return FileError(kind, f"{ERROR_TEXT.get(kind, kind)}（{path}）", detail)


def query(link, path: str, timeout: float = 5.0) -> dict:
    cmd, payload = link.request(CMD_FILE_QUERY, nc4.str_u16(path),
                                expect={CMD_FILE_QUERY_RSP, CMD_FILE_ERROR_RSP},
                                timeout=timeout)
    if cmd == CMD_FILE_ERROR_RSP:
        raise parse_error(payload)
    return _parse_query(payload)


def scan(link, target: int = 0) -> None:
    """0x200B FILE_SCAN（schema：target u8）。冇回覆；送完要 poll manifest。"""
    link.send(CMD_FILE_SCAN, bytes([target & 0xFF]))


def fetch_manifest(link, timeout: float = 10.0) -> dict:
    """下載 /manifest.json 並 parse；格式跟 mp：{path: {"s","h","p"}}。"""
    data = download(link, "/manifest.json", timeout=timeout)
    try:
        obj = json.loads(data.decode("utf-8"))
    except (json.JSONDecodeError, UnicodeDecodeError) as e:
        raise FileError("bad_reply", f"manifest 唔係合法 JSON：{e}") from e
    if not isinstance(obj, dict):
        raise FileError("bad_reply", "manifest 頂層唔係 object")
    return obj


def upload(link, path: str, data: bytes, chunk_size: int = 4096,
           file_id: int = 1,
           progress: Callable[[int, int], None] | None = None) -> dict:
    data = bytes(data)
    total = len(data)
    digest = hashlib.sha256(data).digest()
    payload = struct.pack("<HIH", file_id, total, chunk_size) + digest + nc4.str_u16(path)
    cmd, rsp = link.request(CMD_FILE_BEGIN, payload,
                            expect={CMD_FILE_ACK, CMD_FILE_ERROR_RSP}, timeout=8.0)
    if cmd == CMD_FILE_ERROR_RSP:
        raise parse_error(rsp)

    for off in range(0, total, chunk_size):
        chunk = data[off:off + chunk_size]
        cmd, rsp = link.request(
            CMD_FILE_CHUNK,
            struct.pack("<HI", file_id, off) + chunk,
            expect={CMD_FILE_ACK, CMD_FILE_ERROR_RSP},
            timeout=10.0,
        )
        if cmd == CMD_FILE_ERROR_RSP:
            raise parse_error(rsp)
        got_id, ack_off = struct.unpack_from("<HI", rsp, 0)
        if got_id != file_id or ack_off != off:
            raise FileError("err_offset_mismatch",
                            f"ACK 錯位：id={got_id} off={ack_off} 期望 {off}")
        if progress:
            progress(min(off + len(chunk), total), total)

    cmd, rsp = link.request(CMD_FILE_END, struct.pack("<H", file_id),
                            expect={CMD_FILE_QUERY_RSP, CMD_FILE_ERROR_RSP},
                            timeout=30.0)
    if cmd == CMD_FILE_ERROR_RSP:
        raise parse_error(rsp)
    if progress:
        progress(total, total)
    q = _parse_query(rsp)
    if q["sha256"] != digest:
        raise FileError("err_sha_mismatch",
                        f"裝置回報 SHA 唔同：{q['sha256'].hex()[:12]}… != {digest.hex()[:12]}…")
    return q


def download(link, path: str, chunk_size: int = 4096,
             progress: Callable[[int, int], None] | None = None,
             timeout: float = 10.0) -> bytes:
    q = query(link, path, timeout=timeout)
    total = int(q["size"])
    if not q["exists"]:
        raise FileError("not_found", f"裝置上冇 {path}")
    if total == 0:
        return b""
    buf = bytearray()
    off = 0
    while off < total:
        want = min(chunk_size, total - off)
        payload = nc4.str_u16(path) + struct.pack("<IH", off, want)
        cmd, rsp = link.request(CMD_FILE_READ, payload,
                                expect={CMD_FILE_CHUNK, CMD_FILE_ERROR_RSP},
                                timeout=10.0)
        if cmd == CMD_FILE_ERROR_RSP:
            raise parse_error(rsp)
        if len(rsp) < 6:
            raise FileError("bad_reply", "FILE_READ 回覆太短")
        rid, roff = struct.unpack_from("<HI", rsp, 0)
        chunk = rsp[6:]
        if roff != off:
            raise FileError("err_offset_mismatch", f"READ 錯位：{roff} != {off}")
        if not chunk:
            break
        buf.extend(chunk)
        off += len(chunk)
        if progress:
            progress(off, total)
    if len(buf) != total:
        raise FileError("short_read", f"讀到 {len(buf)}/{total} B（檔案被改咗？）")
    return bytes(buf)


def _simple(link, cmd: int, path: str, timeout: float = 8.0) -> dict:
    rsp_cmd, payload = link.request(cmd, nc4.str_u16(path),
                                    expect={CMD_FILE_QUERY_RSP, CMD_FILE_ERROR_RSP},
                                    timeout=timeout)
    if rsp_cmd == CMD_FILE_ERROR_RSP:
        raise parse_error(payload)
    return _parse_query(payload)


def delete(link, path: str) -> dict:
    return _simple(link, CMD_FILE_DELETE, path)


def confirm(link, path: str) -> dict:
    return _simple(link, CMD_FILE_CONFIRM, path)


def undo(link, path: str) -> dict:
    return _simple(link, CMD_FILE_UNDO, path)
