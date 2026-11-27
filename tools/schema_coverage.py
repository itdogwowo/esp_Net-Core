#!/usr/bin/env python3
"""S6：schema 覆蓋率報告——字典宣告的每個 key，喺真實 config 檔案裡面對唔對得上。

    python3 tools/schema_coverage.py                 # 全部 project／preset 摘要
    python3 tools/schema_coverage.py --config ports/project/S3/ESP32-S3-Matrix-LED-Textkit/data/config -v
    python3 tools/schema_coverage.py --write docs/03_notes/schema-coverage.md

為什麼要有它：字典（schema/state-keys.json）同實際 JSON 係兩份資料；
「覆蓋率」唔可以靠感覺，要逐個 path resolve。呢個工具就係嗰把尺：
  hit     = path 存在且型別對
  missing = path 唔存在（未寫／未支援；struct 類容許）
  type    = path 存在但型別唔對（要修）
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCHEMA = ROOT / "schema" / "state-keys.json"

TOK = re.compile(r"([A-Za-z0-9_\-]+)(?:\[(\d+)\])?")


def resolve(doc: dict, path: str):
    cur = doc
    for part in path.split("."):
        m = TOK.fullmatch(part)
        if not m:
            return None
        name, idx = m.group(1), m.group(2)
        if not isinstance(cur, dict) or name not in cur:
            return None
        cur = cur[name]
        if idx is not None:
            i = int(idx)
            if not isinstance(cur, list) or i >= len(cur):
                return None
            cur = cur[i]
    return cur


def type_ok(typ: str, v) -> bool:
    # 空字串 = 未設定（cID "" ＝由 MAC 推導）
    if v == "" and typ != "str":
        return True
    # list = 衍生值（route mask 由 out[] 算、affinity 由 [c0,c1] 算、I80 count 由 data[] 算）
    if isinstance(v, list) and typ != "str":
        return True
    if typ == "str":
        return isinstance(v, str)
    if typ == "bool":
        return isinstance(v, bool) or (isinstance(v, int) and v in (0, 1))
    if typ == "enum":
        return isinstance(v, str) or (isinstance(v, int) and not isinstance(v, bool))
    return isinstance(v, int) and not isinstance(v, bool)


def load_files(cfg_dir: Path) -> dict:
    docs = {}
    for f in cfg_dir.glob("*.json"):
        try:
            docs[f.name] = json.loads(f.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            docs[f.name] = None
    return docs


def load_with_hw(cfg: Path) -> dict:
    docs = load_files(cfg)
    # ports/project/<chip>/<board>/data/config → ports/hardware/<chip>/<board>/
    # project.json 的 "hardware":"S3/ESP32-S3-Matrix" 才是 hardware 目錄名
    # （project 名可以同 board 名唔同，例如 …-LED-Textkit）。
    proj = cfg.resolve().parent.parent
    pj = proj / "project.json"
    hw = None
    if pj.is_file():
        try:
            hwref = json.loads(pj.read_text(encoding="utf-8")).get("hardware", "")
            if hwref:
                hw = ROOT / "ports" / "hardware" / hwref
        except json.JSONDecodeError:
            hw = None
    if hw is not None and hw.is_dir():
            for name, doc in load_files(hw).items():
                docs.setdefault(name, doc)      # project 優先；hardware 只補缺
    return docs


def check(docs: dict, keys: list) -> dict:
    stat = {}
    for k in keys:
        path = k.get("path", "")
        if ":" not in path:
            continue
        fname, rest = path.split(":", 1)
        doc = docs.get(fname)
        unit = k["unit"]
        s = stat.setdefault(unit, {"keys": 0, "hit": 0, "missing": 0, "type": 0})
        s["keys"] += 1
        if doc is None:
            s["missing"] += 1
            continue
        v = resolve(doc, rest)
        if v is None:
            s["missing"] += 1
        elif not type_ok(k["type"], v):
            s["type"] += 1
        else:
            s["hit"] += 1
    return stat


def configs() -> list[Path]:
    out = sorted((ROOT / "ports/project").glob("*/*/data/config"))
    out += sorted((ROOT / "ports/presets").glob("*/config"))
    out += sorted((ROOT / "ports/presets").glob("*/data/config"))
    return out


def default_for(k: dict):
    if k["type"] == "str":
        return ""
    if k["type"] == "bool":
        return False
    if k["type"] == "enum":
        return k["values"][0]
    return k.get("min", 0)


def write_template(out: Path, keys: list) -> int:
    out = out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    files: dict[str, dict] = {}
    for k in keys:
        path = k.get("path", "")
        if ":" not in path:
            continue
        fname, rest = path.split(":", 1)
        doc = files.setdefault(fname, {"version": 1,
                                       "_comment": ["由 tools/schema_coverage.py --template 生成；"
                                                    "全部 key 都列出、值=預設（enable=0）。"]})
        cur = doc
        parts = rest.split(".")
        for j, part in enumerate(parts):
            m = TOK.fullmatch(part)
            name, idx = m.group(1), m.group(2)
            last = j == len(parts) - 1
            if idx is not None:
                i = int(idx)
                lst = cur.setdefault(name, [])
                while len(lst) <= i:
                    lst.append({})
                if last:
                    lst[i] = default_for(k)
                else:
                    cur = lst[i]
            elif last:
                cur[name] = default_for(k)
            else:
                nxt = cur.setdefault(name, {})
                if not isinstance(nxt, dict):
                    nxt = {}
                    cur[name] = nxt
                cur = nxt
    for fname, doc in files.items():
        (out / fname).write_text(json.dumps(doc, ensure_ascii=False, indent=2) + "\n",
                                 encoding="utf-8")
    print(f"wrote {out}（{len(files)} 個檔）")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", type=Path, default=None)
    ap.add_argument("--write", type=Path, default=None)
    ap.add_argument("--template", type=Path, default=None,
                    help="由 schema 生成「全部 key 都列出、enable/值=預設」模板 JSON")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()

    schema = json.loads(SCHEMA.read_text(encoding="utf-8"))
    keys = schema["keys"]
    unit_names = {u["id"]: u["name"] for u in schema["units"]}

    if a.template is not None:
        return write_template(a.template, keys)

    targets = [a.config] if a.config else configs()
    lines = []
    bad = 0
    for cfg in targets:
        if not cfg.is_dir():
            continue
        stat = check(load_with_hw(cfg), keys)
        total = {k: sum(s[k] for s in stat.values()) for k in ("keys", "hit", "missing", "type")}
        cfg = cfg.resolve()
        label = str(cfg.relative_to(ROOT)) if cfg.is_relative_to(ROOT) else str(cfg)
        lines.append(f"### `{label}`\n")
        lines.append("| unit | keys | hit | missing | type |")
        lines.append("|---|---:|---:|---:|---:|")
        for uid in sorted(stat):
            s = stat[uid]
            lines.append(f"| {unit_names.get(uid, uid)} | {s['keys']} | {s['hit']} | {s['missing']} | {s['type']} |")
        lines.append(f"| **合計** | **{total['keys']}** | **{total['hit']}** | **{total['missing']}** | **{total['type']}** |")
        lines.append("")
        bad += total["type"]
        if a.verbose:
            print(f"{label}: hit {total['hit']}/{total['keys']}、missing {total['missing']}、type {total['type']}")
            for uid in sorted(stat):
                s = stat[uid]
                print(f"  {unit_names.get(uid, uid):<8} {s['hit']}/{s['keys']}  missing {s['missing']}  type {s['type']}")

    if not a.verbose:
        for ln in lines:
            print(ln)
    if a.write:
        a.write = a.write.resolve()
        a.write.parent.mkdir(parents=True, exist_ok=True)
        a.write.write_text(
            "# schema 覆蓋率報告（自動生成）\n\n"
            "> `python3 tools/schema_coverage.py --write docs/03_notes/schema-coverage.md`\n\n"
            + "\n".join(lines),
            encoding="utf-8")
        print(f"wrote {a.write.relative_to(ROOT)}")
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
