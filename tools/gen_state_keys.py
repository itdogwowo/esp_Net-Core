#!/usr/bin/env python3
"""由 schema/state-keys.json 生成 NCSTATE 的 C 檔（schema 唯一 source of truth）。

生成三份（全部唔准手改）：
  1. include/nc_state_keys.h         key id＋unit id（C 名字）
  2. include/nc_state_schema_data.h  runtime 表的型別／extern／counts
  3. nc_state_schema_data.c          runtime 表（id／unit／type／min／max／enum）

「加／減能力」的做法：改 schema json（加 unit／加 key／刪 key）→ 跑一次呢個工具
→ rebuild。firmware 其他地方唔使改（查表、驗範圍、讀寫都經 nc_state_schema／api）。

用法：
  python3 tools/gen_state_keys.py            # 生成／更新三份檔
  python3 tools/gen_state_keys.py --check    # 驗三份檔同 schema 同步（run_all 用）
"""
from __future__ import annotations

import argparse
import json
import re
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCHEMA = ROOT / "schema" / "state-keys.json"
INC = ROOT / "firmware/components/sys/state/include"
SRC = ROOT / "firmware/components/sys/state"
F_KEYS = INC / "nc_state_keys.h"
F_DATA_H = INC / "nc_state_schema_data.h"
F_DATA_C = SRC / "nc_state_schema_data.c"

# schema type → (blob type, C 型別範圍, 預設 min/max)
# blob type 要同 nc_state_blob.h 的 nc_state_type_t 一致。
TYPES = {
    "u8":   {"blob": "NC_STATE_T_UINT",  "lo": 0,           "hi": 255},
    "u16":  {"blob": "NC_STATE_T_UINT",  "lo": 0,           "hi": 65535},
    "u32":  {"blob": "NC_STATE_T_UINT",  "lo": 0,           "hi": 4294967295},
    "i8":   {"blob": "NC_STATE_T_SINT",  "lo": -128,        "hi": 127},
    "i32":  {"blob": "NC_STATE_T_SINT",  "lo": -2147483648, "hi": 2147483647},
    "bool": {"blob": "NC_STATE_T_BOOL",  "lo": 0,           "hi": 1},
    "enum": {"blob": "NC_STATE_T_UINT",  "lo": 0,           "hi": None},   # hi = len(values)-1
    "str":  {"blob": "NC_STATE_T_BYTES", "lo": 0,           "hi": None},   # 用 max_len
}
C_IDENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
KEY_NAME = re.compile(r"^NC_KEY_[A-Z0-9_]+$")


class SchemaError(SystemExit):
    pass


def die(msg: str) -> None:
    raise SchemaError(f"schema 錯誤：{msg}")


def cstr(s: str) -> str:
    """Python 字串 → C 字串 literal（UTF-8 直接寫，只轉義 " 與 \\）。"""
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def load_schema() -> dict:
    data = json.loads(SCHEMA.read_text(encoding="utf-8"))

    version = int(data.get("version", 1))
    if not (1 <= version <= 65535):
        die(f"version 超出 u16：{version}")

    units_raw = data.get("units") or []
    if not units_raw:
        die("一定要有 units[]（unit = 能力群組；key 要引用它）")

    units: list[dict] = []
    unit_by_id: dict[int, dict] = {}
    unit_by_name: dict[str, dict] = {}
    for u in units_raw:
        uid = int(u["id"])
        name = str(u.get("name", ""))
        if not (0 <= uid <= 15):
            die(f"unit id 要 0..15（key id 的 unit nibble）：{u}")
        if uid in unit_by_id:
            die(f"unit id 重複：{uid}")
        if not C_IDENT.match(name):
            die(f"unit name 唔係 C identifier：{name!r}")
        if name in unit_by_name:
            die(f"unit name 重複：{name}")
        rec = {"id": uid, "name": name, "desc": str(u.get("desc", ""))}
        units.append(rec)
        unit_by_id[uid] = rec
        unit_by_name[name] = rec
    units.sort(key=lambda x: x["id"])

    keys: list[dict] = []
    ids: dict[int, str] = {}
    names: set[str] = set()
    for k in data.get("keys") or []:
        kid = int(str(k["id"]), 0)
        if not (0 <= kid <= 0xFFFF):
            die(f"id 超出 u16：{k}")
        if kid in ids:
            die(f"id 重複：0x{kid:04x}（{ids[kid]}／{k.get('name')}）")
        name = str(k["name"])
        if not KEY_NAME.match(name):
            die(f"key name 要 NC_KEY_ 開頭、大寫：{name!r}")
        if name in names:
            die(f"name 重複：{name}")
        typ = str(k.get("type", ""))
        if typ not in TYPES:
            die(f"未知 type：{typ!r}（{name}）")
        uname = str(k.get("unit", ""))
        if uname not in unit_by_name:
            die(f"key {name} 引用了未知 unit：{uname!r}")
        unit = unit_by_name[uname]
        if ((kid >> 8) & 0x0F) != unit["id"]:
            die(f"key {name}（0x{kid:04x}）的 unit nibble="
                f"{(kid >> 8) & 0x0F} 同 unit {uname}（{unit['id']}）唔一致")

        t = TYPES[typ]
        values = k.get("values")
        if typ == "enum":
            if not values or len(values) < 2:
                die(f"enum key {name} 要 values[]（至少 2 個）")
            if len(values) > 256:
                die(f"enum key {name} 太多值（{len(values)}）")
            hi = len(values) - 1
            lo = 0
            for i, v in enumerate(values):
                if not isinstance(v, str) or not v:
                    die(f"enum key {name} 的 values[{i}] 要非空字串")
        elif typ == "str":
            if "max_len" not in k or int(k["max_len"]) <= 0:
                die(f"str key {name} 要 max_len（>0，唔含 NUL）")
            lo, hi = 0, int(k["max_len"])
            if hi > 255:
                die(f"str key {name} 的 max_len 太大（{hi}）")
        else:
            lo = int(k.get("min", t["lo"]))
            hi = int(k.get("max", t["hi"]))
        if t["hi"] is not None and not (t["lo"] <= lo <= t["hi"] and
                                        t["lo"] <= hi <= t["hi"]):
            die(f"key {name} 的 min/max 超出 {typ} 範圍：{lo}..{hi}")
        if lo > hi:
            die(f"key {name} 的 min > max：{lo} > {hi}")
        # int32 表：u32 的合法上限（例如 clock）仍放得進 int32；再大就要改表型別。
        if lo < -2147483648 or hi > 2147483647:
            die(f"key {name} 的範圍放唔入 int32 表：{lo}..{hi}")

        ids[kid] = name
        names.add(name)
        keys.append({
            "id": kid,
            "name": name,
            "unit": unit["id"],
            "unit_name": uname,
            "type": typ,
            "blob": t["blob"],
            "min": lo,
            "max": hi,
            "max_len": int(k.get("max_len", 0)),
            "values": list(values) if values else [],
            "path": str(k.get("path", "")),
            "desc": str(k.get("desc", "")),
        })

    keys.sort(key=lambda x: x["id"])
    # 每個 unit 至少要有一個 key（否則 activation／status 的 unit 名冇意義）
    used = {k["unit"] for k in keys}
    for u in units:
        if u["id"] not in used:
            die(f"unit {u['name']} 冇任何 key（加 key 或刪 unit）")
    return {"version": version, "units": units, "keys": keys}


def schema_crc(data: dict) -> int:
    """schema 內容的 CRC32（唔包 _comment；排序後 canonical 化 → 排版改動唔影響）。"""
    canon = {
        "version": data["version"],
        "units": data["units"],
        "keys": [{k: v for k, v in key.items() if not k.startswith("_")}
                 for key in data["keys"]],
    }
    blob = json.dumps(canon, ensure_ascii=False, sort_keys=True,
                      separators=(",", ":")).encode("utf-8")
    return zlib.crc32(blob) & 0xFFFFFFFF


BANNER = "// **本檔由 tools/gen_state_keys.py 生成，唔好手改**：改 schema/state-keys.json 再跑工具。"


def render_keys(schema: dict) -> str:
    units = schema["units"]
    keys = schema["keys"]
    lines = [
        "// NCSTATE key id／unit id：全專案唯一 source of truth。",
        "//",
        BANNER,
        "// run_all.sh 會用 --check 驗同步；命名規矩：key id = domain(4b)|unit(4b)|index(4b)|field(4b)。",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        f'#define NC_STATE_KEYS_VERSION {schema["version"]}u',
        "",
        "// unit = 能力群組；key id 的 unit nibble 一定要等於呢個值（生成器已驗）。",
        "enum {",
    ]
    for u in units:
        note = f"  /* 0x{u['id']:x} {u['desc']} */" if u["desc"] else ""
        lines.append(f"    NC_UNIT_{u['name'].upper()} = {u['id']},{note}")
    lines += ["};", "", "enum {"]
    for k in keys:
        note = k["desc"] + (f"（{k['path']}）" if k["path"] else "")
        lines.append(f"    /* 0x{k['id']:04x} {note} */")
        lines.append(f"    {k['name']} = 0x{k['id']:04x},")
    lines += ["};", ""]
    return "\n".join(lines)


def render_data_h(schema: dict, crc: int) -> str:
    lines = [
        "// NCSTATE schema 的 runtime 表（宣告）。",
        "//",
        BANNER,
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        '#include "nc_state_blob.h"',
        '#include "nc_state_keys.h"   // NC_UNIT_*／NC_KEY_*',
        "",
        f'#define NC_STATE_SCHEMA_VERSION {schema["version"]}u',
        f'#define NC_STATE_SCHEMA_CRC     0x{crc:08x}u',
        f'#define NC_STATE_KEY_COUNT      {len(schema["keys"])}u',
        f'#define NC_STATE_UNIT_COUNT     {len(schema["units"])}u',
        "",
        "// 一筆 key 的完整描述（id／能力群組／型別／可接受範圍／人讀資料）。",
        "// type 用 nc_state_blob.h 的 NC_STATE_T_*；str 的長度上限在 max_len。",
        "typedef struct {",
        "    uint16_t id;",
        "    uint8_t  unit;        // NC_UNIT_*",
        "    uint8_t  type;        // NC_STATE_T_UINT／SINT／BOOL／BYTES",
        "    int32_t  min;         // 數值範圍（str 用 0..max_len）",
        "    int32_t  max;",
        "    uint16_t max_len;     // str 專用（唔含 NUL；其他 type 為 0）",
        "    uint8_t  n_enum;      // enum 值個數（其他 type 為 0）",
        "    const char *const *enum_names;   // enum：0..n_enum-1 的名字",
        "    const char *name;     // NC_KEY_*",
        "    const char *path;     // 人讀：config JSON 的路徑",
        "    const char *desc;",
        "} nc_state_keydef_t;",
        "",
        "typedef struct {",
        "    uint8_t     id;",
        "    const char *name;",
        "    const char *desc;",
        "} nc_state_unitdef_t;",
        "",
        "extern const nc_state_keydef_t  nc_state_schema_keys[NC_STATE_KEY_COUNT];",
        "extern const nc_state_unitdef_t nc_state_schema_units[NC_STATE_UNIT_COUNT];",
        "",
    ]
    return "\n".join(lines)


def render_data_c(schema: dict) -> str:
    lines = [
        "// NCSTATE schema 的 runtime 表（資料）。",
        "//",
        BANNER,
        '#include "nc_state_schema_data.h"',
        "",
    ]
    for k in schema["keys"]:
        if k["values"]:
            vals = ", ".join(cstr(v) for v in k["values"])
            lines.append(f"static const char *const s_enum_{k['name'].lower()}[] = {{ {vals} }};")
            lines.append("")
    lines.append("const nc_state_keydef_t nc_state_schema_keys[NC_STATE_KEY_COUNT] = {")
    for k in schema["keys"]:
        enum_ref = (f"s_enum_{k['name'].lower()}" if k["values"] else "NULL")
        lines += [
            "    {",
            f"        .id = 0x{k['id']:04x}, .unit = NC_UNIT_{k['unit_name'].upper()},",
            f"        .type = {k['blob']}, .min = {k['min']}, .max = {k['max']},",
            f"        .max_len = {k['max_len']}, .n_enum = {len(k['values'])},",
            f"        .enum_names = {enum_ref},",
            f"        .name = {cstr(k['name'])}, .path = {cstr(k['path'])},",
            f"        .desc = {cstr(k['desc'])},",
            "    },",
        ]
    lines.append("};")
    lines.append("")
    lines.append("const nc_state_unitdef_t nc_state_schema_units[NC_STATE_UNIT_COUNT] = {")
    for u in schema["units"]:
        lines.append(f"    {{ .id = {u['id']}, .name = {cstr(u['name'])}, "
                     f".desc = {cstr(u['desc'])} }},")
    lines.append("};")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    schema = load_schema()
    crc = schema_crc(schema)
    outs = {
        F_KEYS: render_keys(schema),
        F_DATA_H: render_data_h(schema, crc),
        F_DATA_C: render_data_c(schema),
    }

    if args.check:
        bad = 0
        for path, text in outs.items():
            cur = path.read_text(encoding="utf-8") if path.is_file() else ""
            if cur.replace("\r\n", "\n") != text:
                print(f"✘ {path.relative_to(ROOT)} 同 schema 唔同步；"
                      f"跑 python3 tools/gen_state_keys.py", file=sys.stderr)
                bad += 1
        if bad:
            return 1
        print(f"state schema OK（v{schema['version']}、{len(schema['keys'])} key、"
              f"{len(schema['units'])} unit、crc=0x{crc:08x}）")
        return 0

    for path, text in outs.items():
        path.write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote 3 files（v{schema['version']}、{len(schema['keys'])} key、"
          f"{len(schema['units'])} unit、crc=0x{crc:08x}）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
