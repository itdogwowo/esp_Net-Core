#!/usr/bin/env python3
"""檢查／輸出 ports/presets/ 的中央示範設定（靜態、唔需要板）。

    python3 tools/preset_check.py                  # 檢查＋印兩版差異
    python3 tools/preset_check.py --quiet          # 只在有問題時出聲（run_all 用）
    python3 tools/preset_check.py --status         # 印完整狀態報告
    python3 tools/preset_check.py --write-status   # 寫 ports/presets/STATUS.md
    python3 tools/preset_check.py --check-status   # 驗 STATUS.md 冇過期（run_all 用）

驗四件事：
  1. 每個 preset 齊 README／sdkconfig.defaults／data/config 的 6 個 JSON，
     而且 JSON 解析得到、`version` = 1。
  2. schema 有寫到的值一定在範圍內（type／min／max／enum／str 長度）；
     冇寫 = sparse 合法，唔算錯（例：I80.wr 未用就唔寫）。
  3. tasks.json：四層四任務齊、level=4、affinity 剛好一個 1。
  4. 印出各 preset 之間的差異（方便睇「有／冇 PSRAM 兩版差咩」）。

`STATUS.md` 係「這兩個中央示範而家實際有咩值」的快照：由本工具生成、內容決定性
（冇時間戳），`run_all.sh` 用 `--check-status` 擋過期。改 preset／schema 之後跑
`--write-status` 更新。
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PRESETS = ROOT / "ports/presets"
STATUS_PATH = PRESETS / "STATUS.md"
SCHEMA = ROOT / "schema/state-keys.json"
CONFIG_FILES = ["hardware", "system", "network", "tasks", "resources", "driver"]
PIXEL_TASKS = ["render", "pixel_encode_task", "pixel_paint_task", "pixel_effect_task"]
# STATUS.md 想列出的 sdkconfig 開關（冇寫 = 唔顯示）
SDKCONFIG_KEYS = [
    "CONFIG_SPIRAM",
    "CONFIG_NETCORE_PIXEL_LEDS",
    "CONFIG_NETCORE_PIXEL_I80",
    "CONFIG_NETCORE_PIXEL_I80_DIRECT",
    "CONFIG_NETCORE_PIXEL_I80_LANES",
    "CONFIG_NETCORE_PIXEL_I80_PINS",
    "CONFIG_NETCORE_PIXEL_RING_PSRAM",
    "CONFIG_NETCORE_PIXEL_PACK_PSRAM",
    "CONFIG_NETCORE_CONTENT",
    "CONFIG_NETCORE_WIFI",
]


class Check:
    def __init__(self) -> None:
        self.errors: list[str] = []
        self.warns: list[str] = []

    def err(self, msg: str) -> None:
        self.errors.append(msg)

    def warn(self, msg: str) -> None:
        self.warns.append(msg)


def load_schema() -> dict:
    return json.loads(SCHEMA.read_text(encoding="utf-8"))


def schema_crc(schema: dict) -> str:
    """同生成器一致的粗略版本標記：內容 sha256 前 8 位（只作顯示，唔係 C 表的 crc）。"""
    raw = json.dumps(schema, ensure_ascii=False, sort_keys=True).encode()
    return hashlib.sha256(raw).hexdigest()[:8]


def resolve(doc, path: str):
    """'I80.list[0].data' → (True, 值)；路徑唔存在回 (False, None)。"""
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


def value_ok(key: dict, val) -> bool:
    """同 check_value 的規則一致，但只回 bool（STATUS 統計 type_bad 用）。"""
    typ = key["type"]
    if typ == "enum" and isinstance(val, str):
        return val in key.get("values", [])
    if typ in ("u8", "u16", "u32", "i8", "i32", "enum"):
        if val == "":
            return True
        if isinstance(val, bool) or not isinstance(val, int):
            return False
        if typ == "enum":
            return 0 <= val < len(key.get("values", []))
        return key["min"] <= val <= key["max"]
    if typ == "bool":
        return val in (0, 1, True, False)
    if typ == "str":
        return isinstance(val, str) and len(val.encode()) <= key.get("max_len", 0)
    return True


def check_value(ck: Check, where: str, key: dict, val) -> None:
    typ = key["type"]
    # config JSON 的 enum 寫名字（"i80"）；state 存 index（1）。兩個方向都合 schema 才算對。
    if typ == "enum" and isinstance(val, str):
        vals = key.get("values", [])
        if val not in vals:
            ck.err(f"{where}：{key['name']} 要 {vals} 之一，實際 {val!r}")
        return
    if typ in ("u8", "u16", "u32", "i8", "i32", "enum"):
        if val == "":
            return      # 空字串 = 未設定（例：cID "" = 由 MAC 推導）
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

    # build 層（sdkconfig.defaults）同 driver.json 的幾何要一致：
    #   CONFIG_NETCORE_PIXEL_LEDS = 每 lane 燈數（不是總數）
    #   CONFIG_NETCORE_PIXEL_I80_LANES = lane 數
    #   CONFIG_NETCORE_PIXEL_FORMAT = 線材格式
    # 兩者都是「示範」，唔一致會令讀者唔知邊個真；而且 Kconfig 是 driver 缺席時的 fallback。
    sc = _sdkconfig_map(pdir)
    led_px = led.get("px")
    led_lanes = led.get("lanes")
    led_fmt = led.get("format")
    if led_px and "CONFIG_NETCORE_PIXEL_LEDS" in sc:
        try:
            v = int(sc["CONFIG_NETCORE_PIXEL_LEDS"])
            if v != int(led_px):
                ck.err(f"{name}：sdkconfig PIXEL_LEDS={v} 同 driver LED.px={led_px} 唔一致"
                       f"（LEDS 係每 lane 燈數）")
        except ValueError:
            pass
    if led_lanes and "CONFIG_NETCORE_PIXEL_I80_LANES" in sc:
        try:
            v = int(sc["CONFIG_NETCORE_PIXEL_I80_LANES"])
            if v != int(led_lanes):
                ck.err(f"{name}：sdkconfig I80_LANES={v} 同 driver LED.lanes={led_lanes} 唔一致")
        except ValueError:
            pass
    if led_fmt and "CONFIG_NETCORE_PIXEL_FORMAT" in sc:
        v = sc["CONFIG_NETCORE_PIXEL_FORMAT"].strip('"')
        if v and v != led_fmt:
            ck.err(f"{name}：sdkconfig PIXEL_FORMAT={v!r} 同 driver LED.format={led_fmt!r} 唔一致")

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


# ═══════════════════════ STATUS.md ═══════════════════════

def _fmt_val(key: dict, val) -> str:
    if val is None:
        return "—"
    if key["type"] == "enum":
        return str(val)
    return json.dumps(val, ensure_ascii=False)


def _sdkconfig_map(pdir: Path) -> dict[str, str]:
    path = pdir / "sdkconfig.defaults"
    out: dict[str, str] = {}
    if not path.is_file():
        return out
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = line.strip()
        if not line.startswith("CONFIG_") or "=" not in line:
            continue
        sym, val = line.split("=", 1)
        out[sym] = val
    return out


def _sdkconfig_flags(pdir: Path) -> list[tuple[str, str]]:
    path = pdir / "sdkconfig.defaults"
    if not path.is_file():
        return []
    seen: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = line.strip()
        if not line.startswith("CONFIG_") or "=" not in line:
            continue
        sym, val = line.split("=", 1)
        if sym in SDKCONFIG_KEYS:
            seen[sym] = val
    return [(k, seen.get(k, "（未寫）")) for k in SDKCONFIG_KEYS]


def _readme_one_liner(path: Path) -> str:
    """抽 README 的「這份是咩」第一句（決定性，唔含檔名／時間）。"""
    for ln in path.read_text(encoding="utf-8", errors="replace").splitlines():
        s = ln.strip()
        if s.startswith("> **這份是咩**"):
            txt = s[len("> **這份是咩**"):].lstrip("：: ")
            txt = txt.replace("**", "").rstrip()
            if txt and txt[-1] in "，,、；;：:":
                txt = txt[:-1].rstrip() + "…"
            return txt
    for ln in path.read_text(encoding="utf-8", errors="replace").splitlines():
        s = ln.strip()
        if s.startswith(">") and len(s) > 2:
            return s.lstrip("> ").replace("**", "")
    return "（未有說明）"


def build_status(dirs: list[Path], all_docs: dict[str, dict[str, dict]], schema: dict) -> str:
    keys = schema.get("keys", [])
    lines: list[str] = []
    lines.append("# 中央示範設定（`ports/presets/`）— 現況")
    lines.append("")
    lines.append("> **這份係生成物**：`python3 tools/preset_check.py --write-status`。")
    lines.append("> `run_all.sh` 會用 `--check-status` 驗它冇過期；改 preset／schema 之後要重跑。")
    lines.append("> 設計說明：[`README.md`](README.md)｜改 driver：[`driver-config.md`](../../docs/02_guides/driver-config.md)")
    lines.append("")
    lines.append("## Schema")
    lines.append("")
    lines.append(f"- schema v{schema.get('version')}、**{len(keys)} key**、"
                 f"**{len(schema.get('units', []))} unit**、內容 sha256 前 8 位 `{schema_crc(schema)}`")
    lines.append("- 下面嘅 `hit`＝preset JSON 真有寫、而且合 schema 範圍；`missing`＝sparse 未寫"
                 "（多數係未實作的 hardware unit，屬預期）。`type` 一定要 0。")
    lines.append("")
    for pdir in dirs:
        name = pdir.name
        docs = all_docs.get(name, {})
        lines.append(f"## `{name}`")
        lines.append("")
        readme = pdir / "README.md"
        if readme.is_file():
            lines.append(f"_{_readme_one_liner(readme)}_")
        else:
            lines.append("_缺 README.md_")
        lines.append("")

        # 檔案表
        lines.append("| 檔案 | 存在 | JSON | version | bytes |")
        lines.append("|---|---|---|---|---:|")
        for fname in CONFIG_FILES:
            f = pdir / "data/config" / f"{fname}.json"
            exists = "✅" if f.is_file() else "✘"
            doc = docs.get(fname)
            js = "✅" if doc is not None else "✘"
            ver = str(doc.get("version")) if doc is not None else "—"
            size = str(f.stat().st_size) if f.is_file() else "—"
            lines.append(f"| `{fname}.json` | {exists} | {js} | {ver} | {size} |")
        lines.append("")

        # coverage 表
        lines.append("| 檔案 | schema key | hit | missing | type |")
        lines.append("|---|---:|---:|---:|---:|")
        grand = [0, 0, 0, 0]
        for fname in CONFIG_FILES:
            doc = docs.get(fname)
            prefix = f"{fname}.json:"
            total = hit = miss = bad = 0
            for key in keys:
                path = key.get("path", "")
                if not path.startswith(prefix):
                    continue
                total += 1
                ok, val = resolve(doc, path[len(prefix):]) if doc is not None else (False, None)
                if not ok:
                    miss += 1
                else:
                    hit += 1
                    if not isinstance(val, list) and not value_ok(key, val):
                        bad += 1
            grand[0] += total
            grand[1] += hit
            grand[2] += miss
            grand[3] += bad
            lines.append(f"| `{fname}.json` | {total} | {hit} | {miss} | {bad} |")
        lines.append(f"| **合計** | **{grand[0]}** | **{grand[1]}** | **{grand[2]}** | **{grand[3]}** |")
        lines.append("")

        # driver LED 有效值
        led_keys = [k for k in keys if k.get("path", "").startswith("driver.json:LED")]
        lines.append("### driver.json → LED（唯一已實作的 device）")
        lines.append("")
        lines.append("| key | config 路徑 | 實際值 | type／範圍 |")
        lines.append("|---|---|---|---|")
        drv = docs.get("driver", {})
        for key in led_keys:
            fpath = key["path"].split(":", 1)[1]
            ok, val = resolve(drv, fpath)
            rng = key["type"]
            if key.get("values"):
                rng += "：" + "／".join(key["values"])
            else:
                rng += f"（{key['min']}..{key['max']}）"
            lines.append(f"| `{key['name'].replace('NC_KEY_', '')}` | `{fpath}` | "
                         f"{_fmt_val(key, val if ok else None)} | {rng} |")
        lines.append("")
        lines.append("> ⚠ 實際 parser 範圍同 schema 有出入（例：`timing.bit_cells` schema 寫 1..64，"
                     "`nc_cfg_load.c` 只收 0..8）。設定前睇 "
                     "[`driver-config.md`](../../docs/02_guides/driver-config.md) §4。")
        lines.append("")

        # tasks
        lines.append("### tasks.json（四層四任務）")
        lines.append("")
        tasks = docs.get("tasks", {}).get("tasks", [])
        by_name = {t.get("name"): t for t in tasks if isinstance(t, dict)}
        lines.append("| 任務 | level | affinity |")
        lines.append("|---|---:|---|")
        for want in PIXEL_TASKS:
            t = by_name.get(want, {})
            lines.append(f"| `{want}` | {t.get('level', '—')} | "
                         f"{t.get('affinity', '—')} |")
        lines.append("")

        # sdkconfig
        flags = _sdkconfig_flags(pdir)
        if flags:
            lines.append("### sdkconfig.defaults（build 層）")
            lines.append("")
            for sym, val in flags:
                lines.append(f"- `{sym}` = `{val}`")
            lines.append("")

    # 兩版差異
    if len(dirs) == 2:
        a, b = dirs
        lines.append("## 兩版差異（`" + a.name + "` → `" + b.name + "`）")
        lines.append("")
        da, db = all_docs.get(a.name, {}), all_docs.get(b.name, {})
        any_diff = False
        lines.append("| 檔案 | 路徑 | " + a.name + " | " + b.name + " |")
        lines.append("|---|---|---|---|")
        names = [f.name for f in dirs]
        for fname in CONFIG_FILES:
            fa = flatten(da.get(fname, {}))
            fb = flatten(db.get(fname, {}))
            for key in sorted(set(fa) | set(fb)):
                if fa.get(key) == fb.get(key):
                    continue
                any_diff = True
                lines.append(f"| `{fname}.json` | `{key}` | "
                             f"{json.dumps(fa.get(key), ensure_ascii=False)} | "
                             f"{json.dumps(fb.get(key), ensure_ascii=False)} |")
        if not any_diff:
            lines.append("| — | — | — | — |")
        lines.append("")

    # 尾註
    lines.append("## 為什麼 project 的 data/config 唔一定齊 6 個檔")
    lines.append("")
    lines.append("- project 目錄只放「要有自己值」的 seed；`driver.json`／`resources.json` 可以缺，")
    lines.append("  開機會用 ncportgen 生成版／`k_min_resources` 骨架補（runtime 一定有 6 個檔）。")
    lines.append("- `driver.json` 的真正 build-time 真相在 `ports/hardware/<chip>/<board>/driver.json`；")
    lines.append("  project 有鏡像就先嵌，冇就嵌 ncportgen 的 `preset_driver.json`。")
    lines.append("- 檢查 project 鏡像有冇漂移：")
    lines.append("  `python3 tools/sync_hardware_presets.py --check`、"
                 "`python3 tools/sync_driver_presets.py --check`、`python3 tools/board.py check`。")
    lines.append("")
    lines.append("## 驗證")
    lines.append("")
    lines.append("```bash")
    lines.append("python3 tools/preset_check.py              # 結構＋schema 範圍＋四層四任務")
    lines.append("python3 tools/preset_check.py --status     # 呢份報告")
    lines.append("python3 tools/preset_check.py --check-status   # 驗 STATUS.md 冇過期")
    lines.append("python3 tools/schema_coverage.py --config ports/presets/s3-generic-psram/data/config -v")
    lines.append("```")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--status", action="store_true", help="印 STATUS 報告")
    ap.add_argument("--write-status", action="store_true", help="寫 ports/presets/STATUS.md")
    ap.add_argument("--check-status", action="store_true", help="驗 STATUS.md 冇過期")
    args = ap.parse_args()

    schema = load_schema()
    # `_` 開頭 = schema 參考模板（由 schema_coverage.py --template 生成），
    # 唔係可以直接 seed 的 preset，所以唔套「四層四任務」嗰類規則。
    dirs = sorted(d for d in PRESETS.iterdir()
                  if d.is_dir() and (d / "data").is_dir() and not d.name.startswith("_"))
    if not dirs:
        print(f"✘ {PRESETS.relative_to(ROOT)} 冇 preset", file=sys.stderr)
        return 1

    ck = Check()
    all_docs: dict[str, dict[str, dict]] = {}
    for d in dirs:
        all_docs[d.name] = check_preset(ck, d, schema)

    want_status = build_status(dirs, all_docs, schema)

    if args.status:
        print(want_status)
    if args.write_status:
        STATUS_PATH.write_text(want_status, encoding="utf-8")
        print(f"✔ 寫入 {STATUS_PATH.relative_to(ROOT)}（{len(want_status.encode())} B）")
    if args.check_status:
        have = STATUS_PATH.read_text(encoding="utf-8") if STATUS_PATH.is_file() else None
        if have is None:
            ck.err(f"缺 {STATUS_PATH.relative_to(ROOT)}；跑 tools/preset_check.py --write-status")
        elif have != want_status:
            ck.err(f"{STATUS_PATH.relative_to(ROOT)} 過期；跑 tools/preset_check.py --write-status")

    # 差異摘要（兩版）
    if len(dirs) == 2 and not args.quiet and not args.status and not args.write_status:
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
    if not args.quiet and not args.status and not args.write_status:
        print(f"preset_check OK：{len(dirs)} 個 preset、"
              f"{len(schema.get('keys', []))} 個 schema key 對過範圍")
    return 0


if __name__ == "__main__":
    sys.exit(main())
