#!/usr/bin/env python3
"""由 schema/state-keys.json 生成 nc_state_keys.h（key id 唯一 source of truth）。

用法：
  python3 tools/gen_state_keys.py            # 生成／更新 header
  python3 tools/gen_state_keys.py --check    # 確認 header 同 schema 同步（run_all 用）
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCHEMA = ROOT / "schema" / "state-keys.json"
HEADER = ROOT / "firmware/components/sys/state/include/nc_state_keys.h"

TYPES = {"u8", "u16", "u32", "i8", "i32", "bool", "enum", "str"}


def load_schema() -> list[dict]:
    data = json.loads(SCHEMA.read_text(encoding="utf-8"))
    keys = data.get("keys") or []
    ids: dict[int, str] = {}
    names: set[str] = set()
    out: list[dict] = []
    for k in keys:
        kid = int(str(k["id"]), 0)
        name = k["name"]
        typ = k.get("type", "")
        if not (0 <= kid <= 0xFFFF):
            raise SystemExit(f"id 超出 u16：{k}")
        if kid in ids:
            raise SystemExit(f"id 重複：0x{kid:04x}（{ids[kid]}／{name}）")
        if name in names:
            raise SystemExit(f"name 重複：{name}")
        if typ not in TYPES:
            raise SystemExit(f"未知 type：{typ}（{name}）")
        ids[kid] = name
        names.add(name)
        out.append({"id": kid, "name": name, "type": typ,
                    "path": k.get("path", ""), "desc": k.get("desc", "")})
    out.sort(key=lambda x: x["id"])
    return out


def render(keys: list[dict], version: int) -> str:
    lines = [
        "// NCSTATE key id：全專案唯一 source of truth。",
        "//",
        "// **本檔由 tools/gen_state_keys.py 生成，唔好手改**：改 schema/state-keys.json，",
        "// 然後跑 `python3 tools/gen_state_keys.py`；run_all.sh 會用 --check 驗同步。",
        "// 命名規矩：key id = domain(4b) | unit(4b) | index(4b) | field(4b)。",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        f'#define NC_STATE_KEYS_VERSION {version}u',
        "",
        "enum {",
    ]
    for k in keys:
        if k["desc"] or k["path"]:
            note = k["desc"] + (f"（{k['path']}）" if k["path"] else "")
            lines.append(f"    /* 0x{k['id']:04x} {note} */")
        lines.append(f"    {k['name']} = 0x{k['id']:04x},")
    lines += [
        "};",
        "",
    ]
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    data = json.loads(SCHEMA.read_text(encoding="utf-8"))
    keys = load_schema()
    text = render(keys, int(data.get("version", 1)))

    if args.check:
        cur = HEADER.read_text(encoding="utf-8") if HEADER.is_file() else ""
        if cur.replace("\r\n", "\n") != text:
            print(f"✘ {HEADER.relative_to(ROOT)} 同 schema 唔同步；"
                  f"跑 python3 tools/gen_state_keys.py", file=sys.stderr)
            return 1
        print(f"state keys OK（{len(keys)} 個 key、schema v{data.get('version', 1)}）")
        return 0

    HEADER.write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {HEADER.relative_to(ROOT)}（{len(keys)} 個 key）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
