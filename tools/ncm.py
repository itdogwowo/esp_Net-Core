#!/usr/bin/env python3
"""ncm.py —— `.nct` / `.nce` 模組容器的打包與檢視（manifest v1）。

    python3 tools/ncm.py pack --payload fire.wasm --out fire.nce \\
        --kind effect --core 1 --layer 3 --prio 3 --fps 30 --px 676 \\
        --mem 65536 --stack 4096 --budget-us 8000 --cmd 0x3101

    python3 tools/ncm.py pack --payload worker.wasm --out worker.nct \\
        --kind task --core 1 --layer 4 --prio 3

    python3 tools/ncm.py dump fire.nce

檔案佈局（全部 little-endian）：

    +0   header 48 B
    +48  manifest 40 B（flags bit1）
    +88  cmd table（u16 × n，可選，4-byte 對齊）
    +..  payload（wasm 或 aot）

`.nct` = 任務（kind normal）、`.nce` = 效果（kind effect）。副檔名只是給人看的
分類標籤；真正的 kind 以 manifest 為準（沒有 manifest 才用副檔名當預設）。

規則與裝置端 `nc_ncd.c` / `nc_mod.c` 一致：不一致的檔案在板上會被拒絕，
所以這裡先擋一次，錯誤訊息也講清楚（不要等到燒上去才發現）。
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
import zlib

NCM_MAGIC = 0x314D434E          # 'N','C','M','1'
NCD_MAGIC = 0x3144434E          # 'N','C','D','1'
NCM_HDR = 48
NCD_HDR = 40
ABI = 0
FLAG_AOT = 0x0001
FLAG_MANIFEST = 0x0002

KINDS = {"normal": 1, "effect": 2, "service": 1, "task": 1}
TARGETS = {"wasm": 0, "rv32f": 1, "xtensa": 2}

MAX_PX = 4096
MAX_MEM = 128 * 1024
MAX_FPS = 240
MAX_CMDS = 32
MAX_STACK = 32 * 1024
MAX_BUDGET_US = 100_000


def die(msg: str) -> "NoReturn":
    print(f"ncm.py: {msg}", file=sys.stderr)
    raise SystemExit(2)


def align(n: int, a: int) -> int:
    return (n + a - 1) // a * a


def check_kind(kind: int) -> None:
    if kind == 0:
        die("kind=system 不行：載入的任務不能自稱不可卸載的那一層")
    if kind not in (1, 2):
        die(f"kind={kind} 不合法（1=normal 2=effect）")


def build_manifest(args) -> bytes:
    kind = KINDS[args.kind]
    check_kind(kind)
    if not -1 <= args.core <= 1:
        die(f"--core {args.core} 不合法（-1 / 0 / 1）")
    if not 0 <= args.layer <= 7:
        die(f"--layer {args.layer} 不合法（0–7）")
    if not 1 <= args.prio <= 24:
        die(f"--prio {args.prio} 不合法（1–24；0 是 idle）")
    if args.stack != 0 and not 1024 <= args.stack <= MAX_STACK:
        die(f"--stack {args.stack} 不合法（0 或 1024–{MAX_STACK}）")
    if not 0 <= args.fps <= MAX_FPS:
        die(f"--fps {args.fps} 不合法（0–{MAX_FPS}）")
    if not 0 <= args.px <= MAX_PX:
        die(f"--px {args.px} 不合法（0–{MAX_PX}）")
    if not 0 <= args.mem <= MAX_MEM:
        die(f"--mem {args.mem} 不合法（0–{MAX_MEM}）")
    if not 0 <= args.budget_us <= MAX_BUDGET_US:
        die(f"--budget-us {args.budget_us} 不合法（0–{MAX_BUDGET_US}）")
    if len(args.cmd) > MAX_CMDS:
        die(f"指令太多（{len(args.cmd)} > {MAX_CMDS}）")
    if len(set(args.cmd)) != len(args.cmd):
        die("指令表有重複")

    return struct.pack(
        "<IHBbBBHHHIIIIHHI",
        NCD_MAGIC, 1, kind, args.core, args.layer, args.prio,
        args.fps, args.px, 0,                       # flags
        args.stack, args.budget_us, args.mem,
        0,                                          # cmds_off（稍後填）
        len(args.cmd), 0, 0,
    )


META_SIZE = 540
META_TYPES = {"buf": 0, "cmd": 1, "hw": 2, "provide": 3, "module": 4}


def trunc_utf8(s: str, n: int) -> bytes:
    """截到最多 n bytes，而且唔會切斷 UTF-8 序列。

    唔可以寫 `s[:n].encode()`：中文字一個 char 3 bytes，char 截完 encode 會
    超標，而 meta 區塊係**固定 540 B 佈局**（偏移寫死）——超標會令
    slice assignment 拉長個 bytearray，之後所有欄位偏移晒，firmware 讀到
    垃圾 n_provides...（2026-11-17 實測：meta 變 561 B）。
    """
    b = s.encode("utf-8")[:n]
    while b and (b[-1] & 0xC0) == 0x80:      # 收尾嘅 continuation byte 剝走
        b = b[:-1]
    return b


def build_meta(path: str) -> bytes:
    """meta.json → 540 B 二進位 meta 區塊（見 nc_meta.h）。"""
    with open(path, "r", encoding="utf-8") as f:
        m = json.load(f)
    name = trunc_utf8(str(m.get("name", "")), 23)
    desc = trunc_utf8(str(m.get("desc", "")), 63)
    usage = trunc_utf8(str(m.get("usage", "")), 63)
    reqs = list(m.get("requires", []))[:8]
    provs = list(m.get("provides", []))[:8]

    def entry(e) -> bytes:
        t = META_TYPES.get(str(e.get("type", "")), 0)
        flags = 1 if e.get("exclusive") else 0
        b = int(e.get("bytes", 0)) & 0xFFFF
        nm = trunc_utf8(str(e.get("name", "")), 19)
        return struct.pack("<BBH", t, flags, b) + nm.ljust(20, b"\0")

    out = bytearray(META_SIZE)
    out[0:24] = name.ljust(24, b"\0")
    out[24:88] = desc.ljust(64, b"\0")
    out[88:152] = usage.ljust(64, b"\0")
    out[152] = len(reqs)
    out[153] = len(provs)
    for i, e in enumerate(reqs):
        out[156 + i * 24: 156 + (i + 1) * 24] = entry(e)
    for i, e in enumerate(provs):
        out[348 + i * 24: 348 + (i + 1) * 24] = entry(e)
    if len(out) != META_SIZE:
        die(f"內部錯誤：meta 長度 {len(out)} != {META_SIZE}（欄位偏移會錯）")
    return bytes(out)


def parse_meta(blob: bytes, off: int) -> dict:
    def s(a: int, n: int) -> str:
        return blob[a:a + n].split(b"\0", 1)[0].decode("utf-8", "replace")

    def entries(base: int, n: int) -> list:
        out = []
        for i in range(n):
            e = blob[base + i * 24: base + (i + 1) * 24]
            if len(e) < 24:
                break
            t, flags, b = struct.unpack_from("<BBH", e, 0)
            out.append({"type": t, "exclusive": bool(flags & 1), "bytes": b,
                        "name": e[4:24].split(b"\0", 1)[0].decode("utf-8", "replace")})
        return out

    return {
        "name": s(off, 24), "desc": s(off + 24, 64), "usage": s(off + 88, 64),
        "requires": entries(off + 156, blob[off + 152]),
        "provides": entries(off + 348, blob[off + 153]),
    }


def cmd_pack(args) -> int:
    with open(args.payload, "rb") as f:
        payload = f.read()
    if not payload:
        die(f"{args.payload} 是空的")

    manifest = build_manifest(args)
    if len(manifest) != NCD_HDR:
        die(f"內部錯誤：manifest 長度 {len(manifest)} != {NCD_HDR}")

    # 副檔名是給人看的分類標籤：.nct = 任務、.nce = 效果。
    # 沒有寫副檔名就自動補；寫錯就擋（不要產出一個「名字說 task、內容說 effect」的檔）。
    kind = KINDS[args.kind]
    ext = ".nct" if kind == 1 else ".nce"
    if not args.out.endswith((".nct", ".nce")):
        args.out += ext
    elif not args.out.endswith(ext):
        die(f"--kind {args.kind} 的輸出要用 {ext}（你寫了 {args.out}）")

    cmds_off = NCM_HDR + NCD_HDR
    cmd_bytes = b"".join(struct.pack("<H", c) for c in args.cmd)
    meta_bytes = build_meta(args.meta) if args.meta else b""
    meta_off = align(cmds_off + len(cmd_bytes), 4) if meta_bytes else 0
    after = (meta_off + len(meta_bytes)) if meta_bytes else (cmds_off + len(cmd_bytes))
    payload_off = align(after, 4)

    total = payload_off + len(payload)
    crc = zlib.crc32(payload) & 0xFFFFFFFF

    target = TARGETS["wasm"]
    if args.aot:
        if args.target == "wasm":
            die("--aot 需要 --target rv32f|xtensa")
        target = TARGETS[args.target]
    elif args.target != "wasm":
        die("沒有 --aot 就不能指定 --target（會自相矛盾）")

    if args.px == 0 and args.kind == "effect":
        die("--kind effect 需要 --px > 0")

    flags = FLAG_MANIFEST | (FLAG_AOT if args.aot else 0)
    header = struct.pack(
        "<IHHIIIIIIIIII",
        NCM_MAGIC, ABI, flags, total, payload_off, len(payload), crc,
        target, args.mem, args.px, args.fps,
        NCM_HDR, NCD_HDR,
    )
    if len(header) != NCM_HDR:
        die(f"內部錯誤：header 長度 {len(header)} != {NCM_HDR}")

    # cmds_off 只有在真的有序表時才有意義
    if args.cmd:
        manifest = manifest[:28] + struct.pack("<I", cmds_off) + manifest[32:]
    # meta_size（off 34, u16）／meta_off（off 36, u32）
    if meta_bytes:
        manifest = (manifest[:34] + struct.pack("<H", len(meta_bytes)) +
                    struct.pack("<I", meta_off) + manifest[40:])

    blob = bytearray(total)
    blob[0:NCM_HDR] = header
    blob[NCM_HDR:NCM_HDR + NCD_HDR] = manifest
    blob[cmds_off:cmds_off + len(cmd_bytes)] = cmd_bytes
    if meta_bytes:
        blob[meta_off:meta_off + len(meta_bytes)] = meta_bytes
    blob[payload_off:payload_off + len(payload)] = payload

    with open(args.out, "wb") as f:
        f.write(blob)

    print(f"{args.out}: {total} B（payload {len(payload)} @ {payload_off}，"
          f"manifest @ {NCM_HDR}，cmd {len(args.cmd)}，"
          f"meta {len(meta_bytes)} @ {meta_off}，crc {crc:08x}）")
    return 0


def cmd_dump(args) -> int:
    with open(args.file, "rb") as f:
        blob = f.read()
    if len(blob) < NCM_HDR:
        die(f"{args.file}: 太短（{len(blob)} B），不是 .nct / .nce")

    (magic, abi, flags, total, p_off, p_size, p_crc,
     target, mem, px, fps, m_off, m_size) = struct.unpack_from("<IHHIIIIIIIIII", blob, 0)
    print(f"header  : magic=0x{magic:08X} abi={abi} flags=0x{flags:04X}")
    print(f"          total={total} payload_off={p_off} payload_size={p_size} crc={p_crc:08X}")
    print(f"          target={target} mem={mem} px={px} fps={fps}")
    print(f"          manifest_off={m_off} manifest_size={m_size}")
    if magic != NCM_MAGIC:
        die("magic 不對")
    if total != len(blob):
        print(f"  ! 檔長 {len(blob)} != total {total}（被截斷或工具寫錯）")

    if flags & FLAG_MANIFEST:
        ok, m = parse_manifest(blob, m_off, m_size, total)
        if not ok:
            return 1
        kind_name = {1: "normal", 2: "effect"}.get(m["kind"], "?")
        print(f"manifest: kind={kind_name}({m['kind']}) core={m['core']} layer={m['layer']} "
              f"prio={m['prio']} fps={m['fps']} px={m['px']}")
        print(f"          stack={m['stack']} budget_us={m['budget_us']} mem={m['mem']} "
              f"ncmds={m['ncmds']}")
        for i in range(m["ncmds"]):
            cmd = struct.unpack_from("<H", blob, m["cmds_off"] + i * 2)[0]
            print(f"          cmd[{i}] = 0x{cmd:04X}")

        meta_size = struct.unpack_from("<H", blob, m_off + 34)[0]
        meta_off = struct.unpack_from("<I", blob, m_off + 36)[0]
        if meta_off and meta_size >= META_SIZE:
            meta = parse_meta(blob, meta_off)
            print(f"meta    : name={meta['name']} desc={meta['desc']}")
            print(f"          usage={meta['usage']}")
            for r in meta["requires"]:
                print(f"          require type={r['type']} name={r['name']} "
                      f"bytes={r['bytes']} exclusive={int(r['exclusive'])}")
            for p in meta["provides"]:
                print(f"          provide type={p['type']} name={p['name']}")

    body = blob[p_off:p_off + p_size]
    crc = zlib.crc32(body) & 0xFFFFFFFF
    print(f"payload : {p_size} B crc={crc:08X} → {'OK' if crc == p_crc else '不符！'}")
    return 0 if crc == p_crc else 1


def parse_manifest(blob: bytes, off: int, size: int, total: int):
    if off < NCM_HDR or size < NCD_HDR or off + size > total:
        print("  ! manifest 區塊長度不對", file=sys.stderr)
        return False, None
    (magic, ver, kind, core, layer, prio, fps, px, mflags,
     stack, budget, mem, cmds_off, ncmds, rsvd, rsvd2) = struct.unpack_from(
        "<IHBbBBHHHIIIIHHI", blob, off)
    print(f"  manifest magic=0x{magic:08X} ver={ver} flags=0x{mflags:04X} rsvd={rsvd}/{rsvd2}")
    if magic != NCD_MAGIC:
        print("  ! manifest magic 不對", file=sys.stderr)
        return False, None
    if ver != 1:
        print(f"  ! manifest 版本 {ver}（只認 1）", file=sys.stderr)
        return False, None
    if kind not in (1, 2):
        print(f"  ! kind={kind} 不合法（1=normal 2=effect）", file=sys.stderr)
        return False, None
    if ncmds and (cmds_off == 0 or cmds_off + ncmds * 2 > total):
        print("  ! 指令表超出檔案", file=sys.stderr)
        return False, None
    return True, {
        "kind": kind, "core": core, "layer": layer, "prio": prio,
        "fps": fps, "px": px, "stack": stack, "budget_us": budget,
        "mem": mem, "cmds_off": cmds_off, "ncmds": ncmds,
    }


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=".nct / .nce 打包 / 檢視")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("pack", help="把 payload 包成 .nct / .nce（附 manifest）")
    p.add_argument("--payload", required=True, help="wasm 或 aot 檔")
    p.add_argument("--out", required=True, help="輸出檔（.nct = 任務、.nce = 效果）")
    p.add_argument("--kind", default="effect", choices=sorted(KINDS))
    p.add_argument("--core", type=int, default=-1, help="-1 / 0 / 1")
    p.add_argument("--layer", type=int, default=3)
    p.add_argument("--prio", type=int, default=3)
    p.add_argument("--fps", type=int, default=30)
    p.add_argument("--px", type=int, default=0)
    p.add_argument("--mem", type=int, default=65536, help="線性記憶體 bytes")
    p.add_argument("--stack", type=int, default=4096)
    p.add_argument("--budget-us", type=int, default=8000)
    p.add_argument("--cmd", type=lambda s: int(s, 0), action="append", default=[],
                   help="可重複；例如 --cmd 0x3101")
    p.add_argument("--meta", help="meta.json（name / desc / usage / requires / provides）")
    p.add_argument("--aot", action="store_true")
    p.add_argument("--target", default="wasm", choices=sorted(TARGETS))
    p.set_defaults(func=cmd_pack)

    d = sub.add_parser("dump", help="印出 header / manifest / crc")
    d.add_argument("file")
    d.set_defaults(func=cmd_dump)

    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
