#!/usr/bin/env python3
"""檢查 ports/presets/ 的中央示範設定（靜態、唔需要板）。

    python3 tools/preset_check.py            # 檢查＋印兩版差異
    python3 tools/preset_check.py --quiet    # 只在有問題時出聲

驗四件事：
  1. 每個 preset 齊 README／sdkconfig.defaults／data/config 的 6 個 JSON，
     而且 JSON 解析得到、`version` = 1。
  2. schema 有寫到的值一定在範圍內（type／min／max／enum／str 長度）；
     冇寫 = sparse 合法，唔算錯（例：I80.wr 未用就唔寫）。
  3. tasks.json：四層四任務齊、level=4、affinity 剛好一個 1。
  4. 印出各 preset 之間的差異（方便睇「有／冇 PSRAM 兩版差咩」）。
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PRESETS = ROOT / "ports/presets"
SCHEMA = ROOT / "schema/state-keys.json"
CONFIG_FILES = ["hardware", "system", "network", "tasks", "resources", "driver"]
PIXEL_TASKS = ["render", "pixel_encode_task", "pixel_paint_task", "pixel_effect_task"]


class Check:
    def __init__(self) -> None:
        self.errors: list[str] = []
        self.warns: list[str] = []

    def err(self, msg: str) -> None:
        self.errors.append(msg)

    def warn(self, msg: str) -> None:
        self.warns.append(msg)


def resolve(doc, path: str):
    """'I80.list[0].data' → 值；路徑唔存在回 (False, None)。"""
    cur = doc
    for tok in path.split("."):
        idx = None
        if tok.endswith("]") and "[" in tok:
            tok, idx_s = tok[:-1].split("[", 1)
            try:
                idx = int(idx_s)
            except ValueError:
                return False, None
        if not isinstance(cur, dict) or tok not in cur:
            return False, None
        cur = cur[tok]
        if idx is not None:
            if not isinstance(cur, list) or idx >= len(cur):
                return False, None
            cur = cur[idx]
    return True, cur


def check_value(ck: Check, where: str, key: dict, val) -> None:
    typ = key["type"]
    # config JSON 的 enum 寫名字（"i80"）；state 存 index（1）。兩個方向都合 schema 才算對。
    if typ == "enum" and isinstance(val, str):
        vals = key.get("values", [])
        if val not in vals:
            ck.err(f"{where}：{key['name']} 要 {vals} 之一，實際 {val!r}")
        return
    if typ in ("u8", "u16", "u32", "i8", "i32", "enum"):
        if isinstance(val, bool) or not isinstance(val, int):
            ck.err(f"{where}：{key['name']} 要整數，實際 {val!r}")
            return
        if typ == "enum":
            if not (0 <= val < len(key.get("values", []))):
                ck.err(f"{where}：{key['name']} 要 0..{len(key.get('values', [])) - 1}，實際 {val}")
            return
        if not (key["min"] <= val <= key["max"]):
            ck.err(f"{where}：{key['name']} 要 {key['min']}..{key['max']}，實際 {val}")
    elif typ == "bool":
        if val not in (0, 1, True, False):
            ck.err(f"{where}：{key['name']} 要 0／1，實際 {val!r}")
    elif typ == "str":
        if not isinstance(val, str):
            ck.err(f"{where}：{key['name']} 要字串，實際 {val!r}")
        elif len(val.encode()) > key.get("max_len", 0):
            ck.err(f"{where}：{key['name']} 太長（{len(val.encode())} > {key['max_len']}）")


def check_preset(ck: Check, pdir: Path, schema: dict) -> dict[str, dict]:
    name = pdir.name
    if not (pdir / "README.md").is_file():
        ck.err(f"{name}：缺 README.md")
    if not (pdir / "sdkconfig.defaults").is_file():
        ck.err(f"{name}：缺 sdkconfig.defaults")

    docs: dict[str, dict] = {}
    for fname in CONFIG_FILES:
        f = pdir / "data/config" / f"{fname}.json"
        if not f.is_file():
            ck.err(f"{name}：缺 data/config/{fname}.json")
            continue
        try:
            doc = json.loads(f.read_text(encoding="utf-8"))
        except json.JSONDecodeError as e:
            ck.err(f"{name}/{fname}.json：JSON 壞（{e}）")
            continue
        if doc.get("version") != 1:
            ck.err(f"{name}/{fname}.json：version 要 1，實際 {doc.get('version')!r}")
        docs[fname] = doc

    # schema 範圍：有寫的值一定合規
    for key in schema.get("keys", []):
        path = key.get("path", "")
        if ":" not in path:
            continue
        fname, fpath = path.split(":", 1)
        short = fname.replace(".json", "")
        doc = docs.get(short)
        if doc is None:
            continue
        ok, val = resolve(doc, fpath)
        if ok and not isinstance(val, list):
            check_value(ck, f"{name}/{short}.json", key, val)

    # tasks：四層四任務
    tasks = docs.get("tasks", {}).get("tasks", [])
    by_name = {t.get("name"): t for t in tasks if isinstance(t, dict)}
    for want in PIXEL_TASKS:
        t = by_name.get(want)
        if t is None:
            ck.err(f"{name}/tasks.json：缺 {want}")
            continue
        if t.get("level") != 4:
            ck.err(f"{name}/tasks.json：{want} level 要 4，實際 {t.get('level')!r}")
        aff = t.get("affinity")
        if not (isinstance(aff, list) and len(aff) == 2 and sum(1 for x in aff if x == 1) == 1):
            ck.err(f"{name}/tasks.json：{want} affinity 要剛好一個 1，實際 {aff!r}")

    # LED 啟用時，px／lanes 不可為 0（schema 已擋，這裡再講一次原因）
    led = docs.get("driver", {}).get("LED", {})
    if led.get("enable") in (1, True) and led.get("px", 0) <= 0:
        ck.err(f"{name}/driver.json：LED.enable=1 但 px={led.get('px')}")

    return docs


def flatten(doc: dict, prefix: str = "") -> dict[str, object]:
    out: dict[str, object] = {}
    for k, v in doc.items():
        if k.startswith("_"):
            continue
        key = f"{prefix}{k}"
        if isinstance(v, dict):
            out.update(flatten(v, key + "."))
        else:
            out[key] = v
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    schema = json.loads(SCHEMA.read_text(encoding="utf-8"))
    dirs = sorted(d for d in PRESETS.iterdir()
                  if d.is_dir() and (d / "data").is_dir())
    if not dirs:
        print(f"✘ {PRESETS.relative_to(ROOT)} 冇 preset", file=sys.stderr)
        return 1

    ck = Check()
    all_docs: dict[str, dict[str, dict]] = {}
    for d in dirs:
        all_docs[d.name] = check_preset(ck, d, schema)

    # 差異摘要（兩版）
    if len(dirs) == 2 and not args.quiet:
        a, b = dirs
        for fname in CONFIG_FILES:
            fa = flatten(all_docs.get(a.name, {}).get(fname, {}))
            fb = flatten(all_docs.get(b.name, {}).get(fname, {}))
            diffs = [k for k in sorted(set(fa) | set(fb)) if fa.get(k) != fb.get(k)]
            if diffs:
                print(f"差異 {fname}.json（{a.name} vs {b.name}）：")
                for k in diffs:
                    print(f"  {k}: {fa.get(k)!r} → {fb.get(k)!r}")

    for w in ck.warns:
        print(f"△ {w}")
    for e in ck.errors:
        print(f"✘ {e}", file=sys.stderr)
    if ck.errors:
        print(f"—— {len(ck.errors)} 個問題", file=sys.stderr)
        return 1
    if not args.quiet:
        print(f"preset_check OK：{len(dirs)} 個 preset、"
              f"{len(schema.get('keys', []))} 個 schema key 對過範圍")
    return 0


if __name__ == "__main__":
    sys.exit(main())
