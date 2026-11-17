"""測試用：整一個合法 / 可破壞的 NCM 容器（.nct）bytes。

唔 import tools/ncm.py（唔想測試依賴 repo 工具路徑），直接照格式砌。
"""
from __future__ import annotations

import struct
import zlib

NCM_MAGIC = 0x314D434E
NCD_MAGIC = 0x3144434E
FLAG_MANIFEST = 0x0002


def _meta_blob(name="test-mod", desc="測試模組", usage="放 /tasks 再開機",
               requires=(), provides=()):
    out = bytearray(540)
    def put(off, s, n):
        b = s.encode("utf-8")[:n]
        out[off:off + len(b)] = b

    put(0, name, 23)
    put(24, desc, 63)
    put(88, usage, 63)
    out[152] = len(requires)
    out[153] = len(provides)

    def entry(base, i, e):
        typ = e[0]
        exclusive = e[1] if len(e) > 1 else False
        nbytes = e[2] if len(e) > 2 else 0
        nm = e[3] if len(e) > 3 else ""
        off = base + i * 24
        out[off:off + 4] = struct.pack("<BBH", typ, 1 if exclusive else 0, nbytes)
        put(off + 4, nm, 19)

    for i, e in enumerate(requires):
        entry(156, i, e)
    for i, e in enumerate(provides):
        entry(348, i, e)
    return bytes(out)


def make_container(payload: bytes = b"\0asm\x01\0\0\0",
                   kind: int = 1, core: int = -1, layer: int = 4, prio: int = 3,
                   fps: int = 1, px: int = 0, mem: int = 65536, stack: int = 4096,
                   budget_us: int = 20000, cmds=(), meta=True,
                   target: int = 0, aot: bool = False) -> bytes:
    ncm_hdr, ncd_hdr = 48, 40
    cmds_off = ncm_hdr + ncd_hdr
    cmd_bytes = b"".join(struct.pack("<H", c) for c in cmds)
    meta_blob = _meta_blob(requires=((2, True, 0, "gpio"),),
                           provides=((3, False, 0, "test.heartbeat"),)) if meta else b""
    meta_off = ((cmds_off + len(cmd_bytes) + 3) // 4) * 4 if meta_blob else 0
    after = (meta_off + len(meta_blob)) if meta_blob else (cmds_off + len(cmd_bytes))
    payload_off = ((after + 3) // 4) * 4
    total = payload_off + len(payload)
    crc = zlib.crc32(payload) & 0xFFFFFFFF
    flags = FLAG_MANIFEST | (1 if aot else 0)

    header = struct.pack("<IHHIIIIIIIIII", NCM_MAGIC, 0, flags, total, payload_off,
                         len(payload), crc, target, mem, px, fps, ncm_hdr, ncd_hdr)
    manifest = struct.pack("<IHBbBBHHHIIIIHHI", NCD_MAGIC, 1, kind, core, layer, prio,
                           fps, px, 0, stack, budget_us, mem, cmds_off if cmds else 0,
                           len(cmds), 0, 0)
    if meta_blob:
        manifest = (manifest[:34] + struct.pack("<H", len(meta_blob))
                    + struct.pack("<I", meta_off) + manifest[40:])

    blob = bytearray(total)
    blob[0:ncm_hdr] = header
    blob[ncm_hdr:ncm_hdr + ncd_hdr] = manifest
    blob[cmds_off:cmds_off + len(cmd_bytes)] = cmd_bytes
    if meta_blob:
        blob[meta_off:meta_off + len(meta_blob)] = meta_blob
    blob[payload_off:payload_off + len(payload)] = payload
    return bytes(blob)
