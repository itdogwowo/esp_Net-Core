#!/usr/bin/env python3
"""NCM 容器檢視器（`.`nct` / `.nce` / 任何帶 NCM magic 的檔）。

只讀檔頭＋manifest＋cmd table＋meta；唔解 wasm payload（只驗 CRC）。
格式權威：`tools/ncm.py`（打包）＋`firmware/components/host/ncm/include/nc_ncd.h`。

回傳 dict（全部可序列化成 JSON）：
    {"valid": bool, "errors": [...], "warnings": [...],
     "file": {...}, "header": {...}, "manifest": {...}|None,
     "meta": {...}|None, "payload": {...}, "hex": "..."}
"""
from __future__ import annotations

import struct
import zlib
from pathlib import Path

NCM_MAGIC = 0x314D434E          # 'N','C','M','1'
NCD_MAGIC = 0x3144434E          # 'N','C','D','1'
NCM_HDR = 48
NCD_HDR = 40
META_SIZE = 540

FLAG_AOT = 0x0001
FLAG_MANIFEST = 0x0002

KIND_NAMES = {0: "system", 1: "task", 2: "effect"}
TARGET_NAMES = {0: "wasm", 1: "rv32f", 2: "xtensa"}
META_TYPE_NAMES = {0: "buf", 1: "cmd", 2: "hw", 3: "provide", 4: "module"}

KNOWN_EXTS = (".nct", ".nce")


def _u8str(b: bytes) -> str:
    return b.split(b"\0", 1)[0].decode("utf-8", "replace")


def _hexdump(b: bytes, limit: int = 96) -> str:
    out = []
    for off in range(0, min(len(b), limit), 16):
        chunk = b[off:off + 16]
        hx = " ".join(f"{x:02x}" for x in chunk)
        txt = "".join(chr(x) if 32 <= x < 127 else "." for x in chunk)
        out.append(f"{off:04x}  {hx:<47}  {txt}")
    return "\n".join(out)


def _parse_meta(blob: bytes, off: int) -> dict:
    def s(base: int, n: int) -> str:
        return _u8str(blob[base:base + n])

    def entries(base: int, n: int) -> list:
        out = []
        for i in range(n):
            e = blob[base + i * 24: base + (i + 1) * 24]
            if len(e) < 24:
                break
            typ, flags, nbytes = struct.unpack_from("<BBH", e, 0)
            out.append({
                "type": typ,
                "type_name": META_TYPE_NAMES.get(typ, f"?{typ}"),
                "exclusive": bool(flags & 1),
                "bytes": nbytes,
                "name": _u8str(e[4:24]),
            })
        return out

    return {
        "name": s(off, 24),
        "desc": s(off + 24, 64),
        "usage": s(off + 88, 64),
        "requires": entries(off + 156, blob[off + 152]),
        "provides": entries(off + 348, blob[off + 153]),
    }


def parse_container(blob: bytes, filename: str = "") -> dict:
    name = Path(filename or "file").name
    ext = Path(name).suffix.lower()
    errors: list[str] = []
    warnings: list[str] = []

    res = {
        "valid": False,
        "errors": errors,
        "warnings": warnings,
        "file": {
            "name": name,
            "ext": ext,
            "ext_known": ext in KNOWN_EXTS,
            "size": len(blob),
        },
        "header": None,
        "manifest": None,
        "meta": None,
        "payload": None,
        "hex": _hexdump(blob),
    }

    if len(blob) < NCM_HDR:
        errors.append(f"檔案太短（{len(blob)} B < 48 B 檔頭）")
        return res

    (magic, abi, flags, total, p_off, p_size, p_crc,
     target, mem, px, fps, m_off, m_size) = struct.unpack_from("<IHHIIIIIIIIII", blob, 0)

    if magic != NCM_MAGIC:
        errors.append(f"magic 不對（0x{magic:08X}，要 0x{NCM_MAGIC:08X}）")
        warnings.append("唔係 NCM 容器；可能係 wasm／aot／其他檔案")
        return res

    header = {
        "magic": f"0x{magic:08X}",
        "abi": abi,
        "flags": f"0x{flags:04X}",
        "flags_list": [
            *(["AOT"] if flags & FLAG_AOT else []),
            *(["MANIFEST"] if flags & FLAG_MANIFEST else []),
        ],
        "total": total,
        "payload_off": p_off,
        "payload_size": p_size,
        "payload_crc": f"0x{p_crc:08X}",
        "target": target,
        "target_name": TARGET_NAMES.get(target, f"?{target}"),
        "mem": mem,
        "px": px,
        "fps": fps,
        "manifest_off": m_off,
        "manifest_size": m_size,
    }
    res["header"] = header

    if ext and not res["file"]["ext_known"]:
        warnings.append(f"副檔名 {ext} 唔係 .nct／.nce；內容雖然係 NCM 容器，"
                        f"但裝置只會掃描 .nct／.nce，唔會載入呢個檔")
    if abi != 0:
        errors.append(f"ABI={abi}（只認 0）")
    if total != len(blob):
        warnings.append(f"檔長 {len(blob)} != header.total {total}"
                        f"（可能被截斷或附加了資料）")
    if p_off + p_size > len(blob):
        errors.append("payload 超出檔案結尾")
    if p_off < NCM_HDR:
        errors.append(f"payload_off {p_off} 落在檔頭內")

    # ── manifest ──
    if flags & FLAG_MANIFEST:
        if m_off < NCM_HDR or m_off + m_size > len(blob) or m_size < NCD_HDR:
            errors.append(f"manifest 區塊長度不對（off={m_off} size={m_size}）")
        else:
            (mmagic, ver, kind, core, layer, prio, mfps, mpx, mflags,
             stack, budget, mmem, cmds_off, ncmds, rsvd, rsvd2) = struct.unpack_from(
                "<IHBbBBHHHIIIIHHI", blob, m_off)
            if mmagic != NCD_MAGIC:
                errors.append(f"manifest magic 不對（0x{mmagic:08X}）")
            if ver != 1:
                errors.append(f"manifest 版本 {ver}（只認 1）")
            if kind not in (1, 2):
                errors.append(f"manifest kind={kind} 不合法（1=task 2=effect）")
            cmds = []
            if ncmds:
                if cmds_off < NCM_HDR or cmds_off + ncmds * 2 > len(blob):
                    errors.append(f"指令表超出檔案（off={cmds_off} n={ncmds}）")
                else:
                    cmds = [struct.unpack_from("<H", blob, cmds_off + i * 2)[0]
                            for i in range(ncmds)]
            res["manifest"] = {
                "magic": f"0x{mmagic:08X}",
                "version": ver,
                "kind": kind,
                "kind_name": KIND_NAMES.get(kind, f"?{kind}"),
                "core": core,
                "layer": layer,
                "prio": prio,
                "fps": mfps,
                "px": mpx,
                "stack": stack,
                "budget_us": budget,
                "mem": mmem,
                "cmds_off": cmds_off,
                "ncmds": ncmds,
                "cmds": cmds,
                "flags": f"0x{mflags:04X}",
                "reserved": f"{rsvd}/{rsvd2}",
            }
            # meta（manifest 尾 34/36 有 meta_size/meta_off）
            meta_size = struct.unpack_from("<H", blob, m_off + 34)[0]
            meta_off = struct.unpack_from("<I", blob, m_off + 36)[0]
            if meta_off and meta_size >= META_SIZE:
                if meta_off + META_SIZE > len(blob):
                    errors.append(f"meta 超出檔案（off={meta_off}）")
                else:
                    res["meta"] = _parse_meta(blob, meta_off)
            elif meta_off:
                warnings.append(f"meta_size={meta_size} < {META_SIZE}，忽略")
    else:
        warnings.append("冇 manifest（flags bit1 未設）：裝置會用副檔名當 kind 預設")

    # ── payload ──
    if 0 <= p_off and p_off + p_size <= len(blob):
        body = blob[p_off:p_off + p_size]
        crc = zlib.crc32(body) & 0xFFFFFFFF
        res["payload"] = {
            "offset": p_off,
            "size": p_size,
            "crc_expected": f"0x{p_crc:08X}",
            "crc_actual": f"0x{crc:08X}",
            "crc_ok": crc == p_crc,
            "aot": bool(flags & FLAG_AOT),
            "format": TARGET_NAMES.get(target, f"?{target}"),
            "head": body[:16].hex(),
        }
        if crc != p_crc:
            errors.append("payload CRC 不符（檔案損壞或工具寫錯）")

    res["valid"] = not errors
    return res
