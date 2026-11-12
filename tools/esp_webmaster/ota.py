#!/usr/bin/env python3
"""NC4 OTA client（0x22xx）。

流程（同 tools/ota.py 一致）：
  CAPS(0x2210) → BEGIN(0x2201) → [WRITE(0x2202) → ACK(0x2204)]* → END(0x2203)
  → STATE_RSP(0x221B) → APPLY(0x2220)

只服務 ESP；唔依賴 mp_Net-Core。
"""
from __future__ import annotations

import hashlib
import struct
import time
from typing import Callable

import nc4
from link import LinkError

CMD_OTA_BEGIN = 0x2201
CMD_OTA_WRITE = 0x2202
CMD_OTA_END = 0x2203
CMD_OTA_ACK = 0x2204
CMD_OTA_CAPS_QUERY = 0x2210
CMD_OTA_CAPS_RSP = 0x2211
CMD_OTA_STATE_RSP = 0x221B
CMD_OTA_ERROR_RSP = 0x221C
CMD_OTA_APPLY = 0x2220

ERROR_NAMES = [
    "err_begin_partition_conflict", "err_begin_no_partition", "err_begin_size_too_big",
    "err_write_offset_mismatch", "err_write_flash_fail", "err_write_chunk_too_big",
    "err_end_validate_fail", "err_end_not_writing", "err_apply_set_boot_fail",
    "err_not_in_verified_state", "err_already_active",
]


class OtaError(RuntimeError):
    def __init__(self, names: list[str], detail: dict | None = None):
        self.names = names
        self.detail = detail or {}
        super().__init__(", ".join(names) or "OTA 失敗")


def _parse_error(payload: bytes) -> OtaError:
    flags = payload[:11]
    names = [ERROR_NAMES[i] for i, v in enumerate(flags) if v]
    detail = {}
    if len(payload) >= 19:
        failed, written = struct.unpack_from("<II", payload, 11)
        detail = {"failed_offset": failed, "written": written}
    return OtaError(names, detail)


def ota_update(link, data: bytes, chunk_size: int = 4096, apply: bool = True,
               delay_ms: int = 500, version: str | None = None,
               progress: Callable[[int, int], None] | None = None) -> dict:
    data = bytes(data)
    if not data:
        raise OtaError(["empty image"])
    digest = hashlib.sha256(data).digest()
    fw_ver = version or ("ota-" + time.strftime("%Y%m%d-%H%M%S"))
    t0 = time.time()

    # CAPS：唔一定回，唔阻流程
    try:
        cmd, rsp = link.request(CMD_OTA_CAPS_QUERY, b"",
                                expect={CMD_OTA_CAPS_RSP}, timeout=1.5)
        if cmd == CMD_OTA_CAPS_RSP and len(rsp) >= 6:
            max_chunk = struct.unpack_from("<H", rsp, 0)[0]
            if max_chunk:
                chunk_size = min(chunk_size, max_chunk)
    except LinkError:
        pass

    payload = struct.pack("<IH", len(data), chunk_size) + digest + nc4.str_u16(fw_ver)
    cmd, rsp = link.request(CMD_OTA_BEGIN, payload,
                            expect={CMD_OTA_ACK, CMD_OTA_ERROR_RSP}, timeout=8.0)
    if cmd == CMD_OTA_ERROR_RSP:
        raise _parse_error(rsp)

    off = 0
    while off < len(data):
        chunk = data[off:off + chunk_size]
        cmd, rsp = link.request(CMD_OTA_WRITE, struct.pack("<I", off) + chunk,
                                expect={CMD_OTA_ACK, CMD_OTA_ERROR_RSP}, timeout=10.0)
        if cmd == CMD_OTA_ERROR_RSP:
            raise _parse_error(rsp)
        ack_off, written = struct.unpack_from("<II", rsp, 0)
        if ack_off != off:
            raise OtaError(["err_write_offset_mismatch"],
                           {"ack": ack_off, "expect": off})
        off = written
        if progress:
            progress(off, len(data))

    cmd, rsp = link.request(CMD_OTA_END, b"",
                            expect={CMD_OTA_STATE_RSP, CMD_OTA_ERROR_RSP}, timeout=30.0)
    if cmd == CMD_OTA_ERROR_RSP:
        raise _parse_error(rsp)
    idle, writing, verified = rsp[0], rsp[1], rsp[2]
    if not verified:
        raise OtaError(["not_verified"], {"idle": idle, "writing": writing})

    if apply:
        try:
            link.request(CMD_OTA_APPLY, struct.pack("<BI", 0, delay_ms),
                         expect={CMD_OTA_STATE_RSP}, timeout=2.0)
        except LinkError:
            pass          # 板子可能即刻 reboot，收唔到回覆係正常

    return {
        "ok": True,
        "size": len(data),
        "sha256": digest.hex(),
        "version": fw_ver,
        "seconds": round(time.time() - t0, 2),
        "applied": bool(apply),
    }
