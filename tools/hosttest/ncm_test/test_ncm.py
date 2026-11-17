#!/usr/bin/env python3
"""ncm.py 的格式標準測試：pack / dump / 副檔名 / magic / manifest / crc。

不需要板子、不需要 IDF；只需要 Python 3。
"""
from __future__ import annotations

import contextlib
import io
import json
import os
import shutil
import struct
import sys
import zlib

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import ncm  # noqa: E402

FAIL = 0


def check(cond: bool, what: str) -> None:
    global FAIL
    print(("  PASS  " if cond else "  FAIL  ") + what)
    if not cond:
        FAIL += 1


def quiet(argv) -> int:
    with contextlib.redirect_stdout(io.StringIO()):
        return ncm.main(argv)


def main() -> int:
    # 用測試目錄內的固定 _tmp（沙箱下 %TEMP% / build 可能不可寫；
    # tempfile.mkdtemp 建的 0700 目錄在沙箱下也會被拒）。
    tmp = os.path.join(ROOT, "tools", "hosttest", "ncm_test", "_tmp")
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp, exist_ok=True)
    payload = bytes(range(64))
    p = os.path.join(tmp, "payload.bin")
    with open(p, "wb") as f:
        f.write(payload)

    # ── 1. pack .nce ──
    nce = os.path.join(tmp, "fire.nce")
    rc = quiet(["pack", "--payload", p, "--out", nce, "--kind", "effect",
                "--px", "64", "--fps", "30", "--cmd", "0x3101"])
    check(rc == 0, "pack .nce 回 0")
    blob = open(nce, "rb").read()
    check(len(blob) >= 48 + 40, "檔案至少 88 B")

    (magic, abi, flags, total, poff, psize, pcrc,
     target, mem, px, fps, moff, msize) = struct.unpack_from("<IHHIIIIIIIIII", blob, 0)
    check(magic == 0x314D434E, "header magic = NCM1")
    check(abi == 0, "abi = 0")
    check(flags & 0x0002, "flags bit1 = manifest")
    check(total == len(blob), "total_size = 檔長")
    check(psize == len(payload), "payload_size = 64")
    check(pcrc == (zlib.crc32(payload) & 0xFFFFFFFF), "payload crc 相符")
    check(target == 0, "target = wasm")
    check(px == 64, "px = 64")
    check(fps == 30, "fps = 30")
    check(moff == 48 and msize == 40, "manifest @48 / 40 B")

    (mmagic, ver, kind, core, layer, prio, mfps, mpx, mflags,
     stack, budget, mmem, cmds_off, ncmds, r1, r2) = struct.unpack_from(
        "<IHBbBBHHHIIIIHHI", blob, moff)
    check(mmagic == 0x3144434E, "manifest magic = NCD1")
    check(ver == 1, "manifest version = 1")
    check(kind == 2, "kind = effect(2)")
    check(ncmds == 1, "ncmds = 1")
    check(cmds_off == 88, "cmds_off = 88")
    check(struct.unpack_from("<H", blob, cmds_off)[0] == 0x3101, "cmd[0] = 0x3101")

    # ── 2. pack .nct ──
    nct = os.path.join(tmp, "worker.nct")
    rc = quiet(["pack", "--payload", p, "--out", nct, "--kind", "task", "--fps", "0"])
    check(rc == 0, "pack .nct 回 0")
    blob2 = open(nct, "rb").read()
    mmagic2, ver2, kind2 = struct.unpack_from("<IHB", blob2, 48)
    check(mmagic2 == 0x3144434E, "manifest magic = NCD1（.nct）")
    check(kind2 == 1, "kind = task(1)")

    # ── 2.5 meta 區塊 ──
    meta_path = os.path.join(tmp, "meta.json")
    with open(meta_path, "w", encoding="utf-8") as f:
        json.dump({
            "name": "fire26",
            "desc": "16x16 fire",
            "usage": "put in /data/pixel/effects",
            "requires": [
                {"type": "buf", "name": "led.frame", "bytes": 2704},
                {"type": "cmd", "name": "0x3101"},
            ],
            "provides": [
                {"type": "provide", "name": "fire26.stats"},
            ],
        }, f)
    nce_meta = os.path.join(tmp, "fire_meta.nce")
    rc = quiet(["pack", "--payload", p, "--out", nce_meta, "--kind", "effect",
                "--px", "64", "--meta", meta_path])
    check(rc == 0, "pack --meta 回 0")
    blob3 = open(nce_meta, "rb").read()
    meta_size = struct.unpack_from("<H", blob3, 48 + 34)[0]
    meta_off = struct.unpack_from("<I", blob3, 48 + 36)[0]
    check(meta_size == 540, "meta_size = 540")
    check(meta_off == 88, "meta_off = 88")
    check(blob3[meta_off:meta_off + 6] == b"fire26", "meta name = fire26")
    check(blob3[meta_off + 152] == 2, "n_requires = 2")
    check(blob3[meta_off + 153] == 1, "n_provides = 1")
    e0 = blob3[meta_off + 156: meta_off + 180]
    check(e0[0] == 0, "requires[0].type = buf")
    check(struct.unpack_from("<H", e0, 2)[0] == 2704, "requires[0].bytes = 2704")
    check(e0[4:24].split(b"\0", 1)[0] == b"led.frame", "requires[0].name = led.frame")
    p0 = blob3[meta_off + 348: meta_off + 372]
    check(p0[0] == 3, "provides[0].type = provide")
    check(p0[4:24].split(b"\0", 1)[0] == b"fire26.stats", "provides[0].name = fire26.stats")

    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = ncm.main(["dump", nce_meta])
    check(rc == 0, "dump --meta 回 0")
    check("fire26" in buf.getvalue(), "dump 印出 meta name")

    # ── 3. 副檔名錯 → 拒絕 ──
    try:
        quiet(["pack", "--payload", p, "--out", os.path.join(tmp, "bad.nct"),
               "--kind", "effect", "--px", "64"])
        check(False, "effect 寫成 .nct 應該被拒")
    except SystemExit as e:
        check(e.code == 2, "effect 寫成 .nct → exit 2")

    # ── 4. dump 回 0 ──
    check(quiet(["dump", nce]) == 0, "dump .nce 回 0")
    check(quiet(["dump", nct]) == 0, "dump .nct 回 0")

    print("\n%s（%d 個失敗）" % ("FAILED" if FAIL else "ALL PASS", FAIL))
    shutil.rmtree(tmp, ignore_errors=True)
    return 1 if FAIL else 0


if __name__ == "__main__":
    raise SystemExit(main())
