#!/usr/bin/env python3
"""解碼 NCSTATE（state.ncst）並同 JSON config 做「前後對比」。

    python3 tools/state_dump.py --file state.ncst
    python3 tools/state_dump.py --file state.ncst --config-dir /tmp/cfg
    python3 tools/state_dump.py --file state.ncst --config-dir /tmp/cfg --json
    python3 tools/state_dump.py --port /dev/cu.usbmodem* --workdir /tmp/state
        # 經 NC4 FILE_READ 由裝置下載 state.ncst＋/config/*.json 再對比

輸出：
  ① header（magic/ver/count/src_hash/len/crc）＋CRC 驗證
  ② 逐 record：key id、schema 名／unit／型別、state 值 vs JSON config 值、是否 override
  ③ 冇入 blob 的 value key（未啟動 unit／條件唔成立）
  ④ 大小：blob bytes vs config JSON bytes
"""
from __future__ import annotations

import argparse
import json
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCHEMA = ROOT / "schema/state-keys.json"
MAGIC = b"NCSTATE\0"
HDR = 26

T_UINT, T_SINT, T_BYTES, T_BOOL = 1, 2, 3, 4


def _crc32(data: bytes) -> int:
    import binascii
    return binascii.crc32(data, 0) & 0xFFFFFFFF


def _varint(buf: bytes, off: int) -> tuple[int, int]:
    v = 0
    shift = 0
    while True:
        b = buf[off]
        off += 1
        v |= (b & 0x7F) << shift
        if not (b & 0x80):
            return v, off
        shift += 7


def _zigzag(v: int) -> int:
    return (v >> 1) ^ -(v & 1)


def decode_blob(buf: bytes) -> dict:
    if len(buf) < HDR or not buf.startswith(MAGIC):
        raise SystemExit("唔係 NCSTATE（magic 唔對）")
    ver, flags, count, src_hash, payload_len, blob_crc = struct.unpack_from("<HHHIII", buf, 8)
    payload = buf[HDR:HDR + payload_len]
    crc_ok = (len(payload) == payload_len and _crc32(payload) == blob_crc)
    recs = []
    off = 0
    for _ in range(count):
        if off >= len(payload):
            break
        typ = payload[off]
        off += 1
        key, off = _varint(payload, off)
        ln, off = _varint(payload, off)
        raw = payload[off:off + ln]
        off += ln
        if typ == T_UINT:
            val, _ = _varint(payload, off - ln)
        elif typ == T_SINT:
            z, _ = _varint(payload, off - ln)
            val = _zigzag(z)
        elif typ == T_BOOL:
            val = bool(raw[0]) if raw else False
        elif typ == T_BYTES:
            val = raw.decode("utf-8", "replace")
        else:
            val = raw.hex()
        recs.append({"type": typ, "key": key, "len": ln, "value": val, "raw": raw.hex()})
    return {"ver": ver, "flags": flags, "count": count, "src_hash": src_hash,
            "payload_len": payload_len, "blob_crc": blob_crc, "crc_ok": crc_ok,
            "records": recs, "total": len(buf)}


def load_schema() -> dict:
    s = json.loads(SCHEMA.read_text(encoding="utf-8"))
    return {int(k["id"], 16): k for k in s["keys"]}


def resolve(doc, path: str):
    cur = doc
    for tok in path.split("."):
        idx = None
        if tok.endswith("]") and "[" in tok:
            tok, i = tok[:-1].split("[", 1)
            idx = int(i)
        if not isinstance(cur, dict) or tok not in cur:
            return False, None
        cur = cur[tok]
        if idx is not None:
            if not isinstance(cur, list) or idx >= len(cur):
                return False, None
            cur = cur[idx]
    return True, cur


def load_configs(cfg_dir: Path) -> dict[str, dict]:
    out = {}
    for f in sorted(cfg_dir.glob("*.json")):
        try:
            out[f.name] = json.loads(f.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            pass
    return out


def fmt_val(defn: dict, val):
    if defn and defn.get("type") == "enum" and isinstance(val, int):
        vals = defn.get("values", [])
        return f"{val}（{vals[val]}）" if 0 <= val < len(vals) else str(val)
    if isinstance(val, bool):
        return "true" if val else "false"
    return json.dumps(val, ensure_ascii=False)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--file", help="本機 state.ncst")
    ap.add_argument("--config-dir", help="config JSON 目錄（用嚟對比）")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--port", help="由裝置下載（需要 NC4）")
    ap.add_argument("--workdir", default="/tmp/state_dump")
    args = ap.parse_args()

    if args.port:
        wd = Path(args.workdir)
        (wd / "config").mkdir(parents=True, exist_ok=True)
        files = ["system", "hardware", "driver", "tasks", "resources", "network"]
        for f in files:
            subprocess.run([sys.executable, str(ROOT / "tools/nc4_file.py"), "--port", args.port,
                            "get", f"/config/{f}.json", str(wd / "config" / f"{f}.json")],
                           check=False, stdout=subprocess.DEVNULL)
        subprocess.run([sys.executable, str(ROOT / "tools/nc4_file.py"), "--port", args.port,
                        "get", "/config/state.ncst", str(wd / "state.ncst")],
                       check=False, stdout=subprocess.DEVNULL)
        args.file = str(wd / "state.ncst")
        args.config_dir = str(wd / "config")

    if not args.file:
        raise SystemExit("要 --file 或 --port")
    buf = Path(args.file).read_bytes()
    blob = decode_blob(buf)
    schema = load_schema()
    configs = load_configs(Path(args.config_dir)) if args.config_dir else {}

    rows = []
    for r in blob["records"]:
        k = schema.get(r["key"])
        row = {"key": r["key"], "name": k["name"] if k else "?",
               "unit": k["unit"] if k else "?", "type": r["type"],
               "class": k.get("class") if k else "?", "value": r["value"],
               "json": None, "json_ok": False, "override": False}
        if k and configs:
            path = k.get("path", "")
            if ":" in path:
                fname, fpath = path.split(":", 1)
                doc = configs.get(fname.replace(".json", "") + ".json", {})
                ok, jv = resolve(doc, fpath)
                row["json_ok"] = ok
                if ok:
                    derived = False
                    if isinstance(jv, str) and k.get("type") == "enum":
                        vals = k.get("values", [])
                        jv = vals.index(jv) if jv in vals else jv
                    elif isinstance(jv, list):
                        # 陣列衍生值（例：I80 data[] → count；route out[] → mask）
                        jv = len(jv)
                        derived = True
                    elif isinstance(jv, str) and k["name"].endswith("_CID"):
                        try:
                            jv = int(jv, 16)
                        except ValueError:
                            pass
                    row["json"] = jv
                    row["derived"] = derived
                    row["override"] = (jv != r["value"]) and not derived
                else:
                    row["derived"] = False
        rows.append(row)

    stored_keys = {r["key"] for r in blob["records"]}
    missing = [k for kid, k in sorted(schema.items())
               if k.get("class") == "value" and kid not in stored_keys]

    if args.json:
        print(json.dumps({"header": {k: blob[k] for k in ("ver", "flags", "count", "src_hash",
                                                          "payload_len", "blob_crc", "crc_ok", "total")},
                          "records": rows,
                          "missing_value_keys": [{"key": int(k["id"], 16), "name": k["name"]}
                                                 for k in missing]}, ensure_ascii=False, indent=2))
        return 0

    b = blob
    print(f"NCSTATE  {args.file}  {b['total']} B")
    print(f"  ver={b['ver']} flags={b['flags']} count={b['count']} "
          f"src_hash=0x{b['src_hash']:08x} payload={b['payload_len']} "
          f"blob_crc=0x{b['blob_crc']:08x} crc={'OK' if b['crc_ok'] else 'BAD'}")
    if configs:
        print(f"\n{'key':>6}  {'name':<26} {'unit':<6} {'state':>12}  {'JSON':>12}  狀態")
        print("-" * 84)
        for row in rows:
            sv = fmt_val(schema.get(row["key"]), row["value"])
            jv = fmt_val(schema.get(row["key"]), row["json"]) if row["json_ok"] else "—"
            if row["override"]:
                flag = "OVERRIDE"
            elif row["json_ok"]:
                flag = "衍生" if row.get("derived") else "同 JSON"
            else:
                flag = "JSON 冇"
            print(f"{row['key']:>6}  {row['name']:<26} {row['unit']:<6} {sv:>12}  {jv:>12}  {flag}")
    else:
        print(f"\n{'key':>6}  {'name':<26} {'unit':<6} value")
        print("-" * 64)
        for row in rows:
            print(f"{row['key']:>6}  {row['name']:<26} {row['unit']:<6} "
                  f"{fmt_val(schema.get(row['key']), row['value'])}")

    print(f"\n有入 blob：{b['count']} 筆；冇入（value class 但未啟動／條件唔成立）：{len(missing)} 筆")
    if missing:
        print("  " + "、".join(k["name"] for k in missing))
    if args.config_dir:
        cfg_files = list(Path(args.config_dir).glob("*.json"))
        cfg_bytes = sum(f.stat().st_size for f in cfg_files)
        print(f"\n大小：state.ncst {b['total']} B（header {HDR} B＋records "
              f"{b['total'] - HDR} B） vs config JSON {cfg_bytes} B（{len(cfg_files)} 檔）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
