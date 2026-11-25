#!/usr/bin/env python3
"""show_upload.py — 上傳 8×8 Show 的 `/pixel` 內容到裝置（經 WebMaster REST）。

只會 PUT 呢幾類檔（唔會刪檔、唔會覆蓋其他 `/pixel`、`/data` 檔案）：
    effects/effects.json   map/*.json   modes/*.json   registry.json
（map／modes 全部掃，唔再寫死 matrix.json／white80.json；一個 project 可以有多組 map）

流程：先在本機驗 JSON（parse／`end_Time` 嚴格遞增／mode 引用的 group、effect 名存在／
`white` 0..100），全部 OK 才逐檔 PUT，最後可選 `/api/reboot` 令 firmware 重新載入。

目前 project 內容是**低亮度呼吸**（`breathe`，`math_now l_max=384` → 8-bit 最高約 24，
`white=0`），避免 80% 白全亮過熱；要改回其他效果只改 data/pixel 再上傳。

用法：
    python3 tools/show_upload.py                       # 預設 --via http://127.0.0.1:8011
    python3 tools/show_upload.py --reboot
    python3 tools/show_upload.py --via http://127.0.0.1:8011 --base /pixel
    python3 tools/show_upload.py --check               # 只做本機檢查，不上傳
    python3 tools/show_upload.py --port /dev/ttyACM0   # 未支援（請用 --via）

為什麼預設 8011：`tools/` 的量測工具（pipe_bench／effect_bench…）都用 WebMaster 的
`--via http://127.0.0.1:8011`；WebMaster 的 `settings.local.json` 可把 port 設成 8011。
"""
from __future__ import annotations

import argparse
import json
import sys
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_SRC = (REPO_ROOT / "ports" / "project" / "S3" / "ESP32-S3-Matrix-Show"
               / "data" / "pixel")
DEFAULT_VIA = "http://127.0.0.1:8011"
DEFAULT_BASE = "/pixel"

# 預設上傳清單：相對於 --src 的 path，同時係 --base 之下的裝置 path。
FILES = (
    "effects/effects.json",
    "map/matrix.json",
    "modes/white80.json",
    "modes/color.json",
    "registry.json",
)


def file_list(src: Path) -> list[str]:
    """實際要上傳的檔：effects／map／registry ＋ 全部 modes/*.json。

    以前寫死 white80／color；16×600 的 project 會有 3 條以上 mode，
    寫死會令上傳失敗（2026-11-22）。
    """
    out: list[str] = []
    if (src / "effects" / "effects.json").is_file():
        out.append("effects/effects.json")
    # map／modes：全部 *.json（唔再寫死 matrix.json／white80.json）。
    # 一個 project 可以有多組 map（matrix／textkit／16x600…），mode 自己引用 mapping 名。
    for sub in ("map", "modes"):
        d = src / sub
        if d.is_dir():
            out.extend(str(p.relative_to(src)) for p in sorted(d.glob("*.json")))
    if (src / "registry.json").is_file():
        out.append("registry.json")
    return out

SEG_TYPES = {"keep", "math_now", "square_wave_now", "pulse_wave", "pulse", "starter"}
HW_TYPES = {"ws2812", "apa102", "pca9685", "pwm", "uartMotor1"}
WRITE_TYPES = {"r", "g", "b", "w", "ww", "rgb", "rgbw", "wwww"}


# ═══════════════════════ 本機 JSON 檢查 ═══════════════════════
def _load_json(path: Path, rel: str, errs: list[str]):
    try:
        return json.loads(path.read_text(encoding="utf-8-sig"))
    except FileNotFoundError:
        errs.append(f"{rel}: 冇呢個檔（{path}）")
    except (json.JSONDecodeError, UnicodeDecodeError) as e:
        errs.append(f"{rel}: JSON parse 失敗：{e}")
    except OSError as e:
        errs.append(f"{rel}: 讀檔失敗：{e}")
    return None


def _is_int(v) -> bool:
    return isinstance(v, int) and not isinstance(v, bool)


def _int_field(obj: dict, key: str, ctx: str, errs: list[str], lo: int, hi: int,
               required: bool = True, default=None):
    """檢查 obj[key] 係 lo..hi 的整數；缺欄位 = required ? 報錯 : 用 default。"""
    if key not in obj:
        if required:
            errs.append(f"{ctx}: 缺 {key}")
        return default
    v = obj[key]
    if not _is_int(v):
        errs.append(f"{ctx}: {key} 要整數（實際 {type(v).__name__}）")
        return default
    if not lo <= v <= hi:
        errs.append(f"{ctx}: {key}={v} 超範圍 {lo}..{hi}")
        return default
    return v


def _str_field(obj: dict, key: str, ctx: str, errs: list[str], required: bool = True,
               max_len: int | None = None):
    if key not in obj:
        if required:
            errs.append(f"{ctx}: 缺 {key}")
        return None
    v = obj[key]
    if not isinstance(v, str) or not v:
        errs.append(f"{ctx}: {key} 要非空字串")
        return None
    if max_len is not None and len(v) > max_len:
        errs.append(f"{ctx}: {key}={v!r} 長過 {max_len} 字（firmware 會截斷）")
    return v


def _bool_field(obj: dict, key: str, ctx: str, errs: list[str], required: bool = True,
                default=None):
    if key not in obj:
        if required:
            errs.append(f"{ctx}: 缺 {key}")
        return default
    v = obj[key]
    if not isinstance(v, bool):
        errs.append(f"{ctx}: {key} 要 true/false")
        return default
    return v


def _slice_ok(s) -> bool:
    """同 firmware `nc_px_slice_parse` 一樣的語義：0～3 段 `start:stop:step`，數字可負。"""
    if not isinstance(s, str):
        return False
    parts = s.split(":")
    if not 1 <= len(parts) <= 3:
        return False
    for p in parts:
        if p == "":
            continue
        if p == "-":
            return False
        body = p[1:] if p.startswith("-") else p
        if not body.isdigit():
            return False
    return True


def _ref_matches(item: dict, ref: str) -> bool:
    """同 firmware `ref_is`：先比 name，全數字再比 id。"""
    if not isinstance(ref, str) or not ref:
        return False
    if item.get("name") == ref:
        return True
    return ref.isdigit() and _is_int(item.get("id")) and item["id"] == int(ref)


def _find_ref(items, ref):
    for it in items:
        if _ref_matches(it, ref):
            return it
    return None


def _validate_effects(doc, rel: str, errs: list[str]):
    """回 name → 效果 dict（只含驗證過的）；順便驗 end_Time 嚴格遞增。"""
    if not isinstance(doc, dict):
        errs.append(f"{rel}: 頂層要 object")
        return {}
    if doc.get("version") != 1:
        errs.append(f"{rel}: version 要 1")
    arr = doc.get("effects")
    if not isinstance(arr, list) or not arr:
        errs.append(f"{rel}: effects 要非空陣列")
        return {}

    by_name: dict[str, dict] = {}
    for i, e in enumerate(arr):
        ctx = f"{rel}: effects[{i}]"
        if not isinstance(e, dict):
            errs.append(f"{ctx}: 要 object")
            continue
        name = _str_field(e, "name", ctx, errs, max_len=23)
        _int_field(e, "id", ctx, errs, 1, 255)
        _int_field(e, "pixel_n", ctx, errs, 1, 65535)
        _int_field(e, "step", ctx, errs, 0, 65535)
        _int_field(e, "spacing", ctx, errs, 0, 65535)
        _int_field(e, "offset", ctx, errs, 0, 65535)
        _int_field(e, "speed", ctx, errs, 0, 65535)
        _bool_field(e, "reverse", ctx, errs)

        prog = e.get("program")
        if not isinstance(prog, list) or not prog:
            errs.append(f"{ctx}: program 要非空陣列")
            continue
        if len(prog) > 16:
            errs.append(f"{ctx}: program 超過 16 段（firmware 只收 16 段）")
            continue  # 仍驗證頭 16 段的下限內容
        prev_end = 0
        ends: list[int] = []
        for j, seg in enumerate(prog):
            sctx = f"{ctx}.program[{j}]"
            if not isinstance(seg, dict):
                errs.append(f"{sctx}: 要 object")
                continue
            stype = seg.get("type")
            if stype not in SEG_TYPES:
                errs.append(f"{sctx}: type={stype!r} 唔係 firmware 收的段型別")
            _int_field(seg, "F", sctx, errs, 0, 65535)
            _int_field(seg, "l_max", sctx, errs, 0, 4095)
            _int_field(seg, "l_lim", sctx, errs, 0, 4095)
            _int_field(seg, "phi", sctx, errs, 0, 4095)
            end = _int_field(seg, "end_Time", sctx, errs, 1, 65535)
            if end is not None:
                if end <= prev_end:
                    errs.append(f"{sctx}: end_Time={end} 必須嚴格遞增（上一個 {prev_end}）")
                prev_end = end
                ends.append(end)
        if name:
            if name in by_name:
                errs.append(f"{ctx}: 效果名 {name!r} 重複（firmware 保留第一個）")
            else:
                e = dict(e)
                e["_ends"] = ends
                by_name[name] = e
    return by_name


def _validate_map(doc, rel: str, errs: list[str]):
    """回 {id,name,groups[]}（groups 用來查 mode 引用）；失敗回 None。"""
    if not isinstance(doc, dict):
        errs.append(f"{rel}: 頂層要 object")
        return None
    if doc.get("version") != 1:
        errs.append(f"{rel}: version 要 1")
    mid = _int_field(doc, "id", rel, errs, 1, 255)
    name = _str_field(doc, "name", rel, errs, max_len=23)
    groups = doc.get("groups")
    if not isinstance(groups, list) or not groups:
        errs.append(f"{rel}: groups 要非空陣列")
        return None
    if len(groups) > 65535:
        errs.append(f"{rel}: groups 超過 65,535 個")
    out_groups = []
    seen_names: set[str] = set()
    seen_ids: set[int] = set()
    for i, g in enumerate(groups):
        ctx = f"{rel}: groups[{i}]"
        if not isinstance(g, dict):
            errs.append(f"{ctx}: 要 object")
            continue
        gid = _int_field(g, "id", ctx, errs, 1, 65535)
        gname = _str_field(g, "name", ctx, errs, max_len=23)
        if gname:
            if gname in seen_names:
                errs.append(f"{ctx}: group 名 {gname!r} 重複")
            seen_names.add(gname)
        if gid is not None:
            if gid in seen_ids:
                errs.append(f"{ctx}: group id {gid} 重複")
            seen_ids.add(gid)
        sels = g.get("sel")
        if not isinstance(sels, list) or not sels:
            errs.append(f"{ctx}: sel 要非空陣列")
            continue
        nsel = 0
        for j, s in enumerate(sels):
            sctx = f"{ctx}.sel[{j}]"
            if not isinstance(s, dict):
                errs.append(f"{sctx}: 要 object")
                continue
            hw = s.get("type")
            if hw not in HW_TYPES:
                errs.append(f"{sctx}: type={hw!r} 唔係 firmware 收的硬體型別")
            sl = s.get("sel")
            if not _slice_ok(sl):
                errs.append(f"{sctx}: sel={sl!r} 唔係合法 slice（例：\":\"／\"0:16\"）")
            nsel += 1
        if gname:
            out_groups.append({"id": gid, "name": gname, "nsel": nsel})
    if name is None:
        return None
    return {"id": mid, "name": name, "groups": out_groups}


def _resolve_group(entry_group, mapping: str | None, maps: list[dict]):
    """mode 的 group 引用 → (map, group, 錯誤)。語義同 firmware `find_group`。"""
    if not isinstance(entry_group, str) or not entry_group:
        return None, None, "缺 group"
    if "." in entry_group:
        mapref, gname = entry_group.split(".", 1)
        mp = _find_ref(maps, mapref)
        if mp is None:
            return None, None, f"map {mapref!r} 唔存在"
    else:
        gname = entry_group
        if mapping:
            mp = _find_ref(maps, mapping)
            if mp is None:
                return None, None, f"mapping {mapping!r} 唔存在"
        elif len(maps) == 1:
            mp = maps[0]
        else:
            return None, None, f"冇 map 前綴，而 mapping 空／唔唯一（有 {len(maps)} 個 map）"
    grp = _find_ref(mp["groups"], gname)
    if grp is None:
        return mp, None, f"group {gname!r} 唔在 map {mp['name']!r}"
    return mp, grp, None


def _validate_mode(doc, rel: str, effect_names: dict, maps: list[dict], errs: list[str]):
    """回 mode dict；失敗回 None。"""
    if not isinstance(doc, dict):
        errs.append(f"{rel}: 頂層要 object")
        return None
    if doc.get("version") != 1:
        errs.append(f"{rel}: version 要 1")
    mid = _int_field(doc, "id", rel, errs, 1, 255)
    name = _str_field(doc, "name", rel, errs, max_len=23)
    _int_field(doc, "index", rel, errs, 1, 255, required=False)
    mapping = doc.get("mapping")
    if not isinstance(mapping, str):
        errs.append(f"{rel}: mapping 要字串（map 的 name 或 id）")
        mapping = None
    elif _find_ref(maps, mapping) is None:
        errs.append(f"{rel}: mapping={mapping!r} 搵唔到對應 map")
    # `white` 係 task-1 show firmware 的欄位；冇寫 = 0（純效果）。
    white = _int_field(doc, "white", rel, errs, 0, 100, required=False)
    if white is None:
        white = 0
    entries = doc.get("map")
    if not isinstance(entries, list) or not entries:
        errs.append(f"{rel}: map 要非空陣列")
        return None
    if len(entries) > 65535:
        errs.append(f"{rel}: map 超過 65,535 項")
    descr = []
    for i, en in enumerate(entries):
        ctx = f"{rel}: map[{i}]"
        if not isinstance(en, dict):
            errs.append(f"{ctx}: 要 object")
            continue
        group_ref = en.get("group")
        effect = en.get("effect")
        write = en.get("write")
        mp, grp, gerr = _resolve_group(group_ref, mapping, maps)
        if gerr:
            errs.append(f"{ctx}: {gerr}")
        if not isinstance(effect, str) or effect not in effect_names:
            errs.append(f"{ctx}: effect={effect!r} 唔在 effects.json（有：{sorted(effect_names)}）")
        if write not in WRITE_TYPES:
            errs.append(f"{ctx}: write={write!r} 唔係 firmware 收的 write（{sorted(WRITE_TYPES)}）")
        if "range" in en and not _slice_ok(en["range"]):
            errs.append(f"{ctx}: range={en['range']!r} 唔係合法 slice")
        if mp is not None and grp is not None:
            descr.append(f"{mp['name']}.{grp['name']} × {effect} × {write}")
    if name is None:
        return None
    return {"id": mid, "name": name, "white": white, "mapping": mapping,
            "descr": descr}


def _validate_registry(doc, rel: str, modes: list[dict], errs: list[str]):
    if not isinstance(doc, dict):
        errs.append(f"{rel}: 頂層要 object")
        return False
    if doc.get("version") != 1:
        errs.append(f"{rel}: version 要 1")
    _bool_field(doc, "auto_play", rel, errs)
    lst = doc.get("list")
    if not isinstance(lst, list):
        errs.append(f"{rel}: list 要陣列")
        return False
    for i, ref in enumerate(lst):
        if not isinstance(ref, str):
            errs.append(f"{rel}: list[{i}] 要字串（mode 名或 id）")
        elif _find_ref(modes, ref) is None:
            errs.append(f"{rel}: list[{i}]={ref!r} 搵唔到對應 mode")
    if not lst:
        errs.append(f"{rel}: list 空（開機冇 mode 播）")
    return True


def check_tree(src: Path, files: list[str] | None = None) -> tuple[list[tuple[str, str]], list[str]]:
    """驗 --src 之下 FILES 的內容；回 [(rel, 摘要)]、[錯誤]。"""
    errs: list[str] = []
    reports: list[tuple[str, str]] = []
    if not src.is_dir():
        return [], [f"來源目錄唔存在：{src}"]

    # 每個檔驗完，只有「呢個檔零錯誤」才印 OK 摘要（有錯就只印 FAIL 行）。
    # 1. effects
    rel = "effects/effects.json"
    before = len(errs)
    doc = _load_json(src / rel, rel, errs)
    effects = _validate_effects(doc, rel, errs) if doc is not None else {}
    if len(errs) == before and effects:
        parts = []
        for e in effects.values():
            ends = "→".join(str(x) for x in e["_ends"])
            parts.append(f"{e['name']} {len(e['_ends'])} 段，end_Time {ends}"
                         f"（嚴格遞增，波長 {e['_ends'][-1] if e['_ends'] else '?'} 幀）")
        reports.append((rel, f"{len(effects)} 個效果：" + "；".join(parts)))

    # 2. map（全部 map/*.json；一個 project 可以有多組）
    maps: list[dict] = []
    for path in sorted((src / "map").glob("*.json")):
        rel = str(path.relative_to(src))
        before = len(errs)
        doc = _load_json(src / rel, rel, errs)
        mp = _validate_map(doc, rel, errs) if doc is not None else None
        if mp is None:
            continue
        maps.append(mp)
        if len(errs) == before:
            gs = "；".join(f"{g['name']}（{g['nsel']} sel）" for g in mp["groups"])
            reports.append((rel, f"id={mp['id']} name={mp['name']}，"
                                 f"{len(mp['groups'])} 個 group：" + gs))
    if not maps:
        errs.append("map: 冇任何 map/*.json（mode 的 mapping 會解析唔到）")

    # 3. modes（全部 modes/*.json；唔再寫死 white80／color）
    modes: list[dict] = []
    for path in sorted((src / "modes").glob("*.json")):
        rel = str(path.relative_to(src))
        before = len(errs)
        doc = _load_json(path, rel, errs)
        if doc is None:
            continue
        m = _validate_mode(doc, rel, effects, maps, errs)
        if m is None:
            continue
        modes.append(m)
        if len(errs) == before:
            reports.append((rel, f"id={m['id']} name={m['name']} white={m['white']}% "
                                 f"mapping={m['mapping']}，{len(m['descr'])} 項："
                                 + "；".join(m["descr"][:8])
                                 + ("…（共 %d 項）" % len(m["descr"]) if len(m["descr"]) > 8 else "")))
    if len(modes) > 1 and modes[0]["name"] == modes[1]["name"]:
        errs.append("modes: mode 名重複（firmware 保留第一個）")
    ids = [m["id"] for m in modes]
    if len(ids) != len(set(ids)):
        errs.append(f"modes: mode id 重複（{sorted(ids)}）")

    # 4. registry
    rel = "registry.json"
    before = len(errs)
    doc = _load_json(src / rel, rel, errs)
    if doc is not None:
        _validate_registry(doc, rel, modes, errs)
        if len(errs) == before:
            reports.append((rel, f"auto_play={doc.get('auto_play')}，"
                                 f"list={doc.get('list')}"))
    return reports, errs


def _print_checks(reports, errs) -> bool:
    for rel, detail in reports:
        print(f"  [OK]   {rel:<28} {detail}")
    for e in errs:
        print(f"  [FAIL] {e}")
    if errs:
        print(f"檢查結果：有錯（{len(errs)} 個問題）")
        return False
    print(f"檢查結果：全部 OK（{len(reports)} 個檔）")
    return True


# ═══════════════════════ 上傳（WebMaster REST）═══════════════════════
class UploadError(RuntimeError):
    pass


def _http(base: str, method: str, path: str, data: bytes | None = None,
          timeout: float = 30.0) -> tuple[int, bytes]:
    url = base.rstrip("/") + path
    req = urllib.request.Request(url, data=data, method=method,
                                 headers={"X-Actor": "ai"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()
    except urllib.error.URLError as e:
        raise UploadError(f"連不上 {base}：{e.reason}（WebMaster 有開嗎？）") from e


def _target(base: str, rel: str) -> str:
    return base + "/" + rel


def _api_file_path(target: str) -> str:
    return "/api/file?path=" + urllib.parse.quote(target, safe="/")


def upload_files(via: str, base: str, src: Path, timeout: float) -> int:
    """逐檔 PUT；回失敗數。"""
    print(f"\n上傳到 {via}（base={base or '/'}）")
    failed = 0
    hinted_dir = False
    for rel in file_list(src):
        target = _target(base, rel)
        local = (src / rel).read_bytes()
        try:
            status, raw = _http(via, "PUT", _api_file_path(target), local, timeout)
        except UploadError as e:
            print(f"  FAIL {target}: {e}")
            return failed + 1  # server 唔通，後面唔使試
        try:
            body = json.loads(raw.decode("utf-8", "replace")) if raw else {}
        except json.JSONDecodeError:
            body = {}
        if status >= 300 or not body.get("ok"):
            detail = body.get("detail") if isinstance(body, dict) else None
            kind = detail.get("kind") if isinstance(detail, dict) else None
            print(f"  FAIL {target}: HTTP {status} {detail or raw[:300]!r}")
            failed += 1
            if kind == "err_write_fail" and rel.startswith(("map/", "modes/")) and not hinted_dir:
                hinted_dir = True
                print("        提示：裝置上 /pixel/map、/pixel/modes 目錄可能未存在"
                      "（FILE_BEGIN 唔會 mkdir 父目錄）。新 firmware 的 seed_dirs()／"
                      "pipeline_task 開機會建；仍失敗就確認燒咗新映像，或先用 MSC 模式手動建目錄。")
            continue
        size = body.get("size")
        sha = str(body.get("sha256") or "")
        size_txt = f"size={size} B" if isinstance(size, int) else "size=（裝置冇回）"
        note = ""
        if isinstance(size, int) and size != len(local):
            note = f"  ⚠ 裝置回報 size={size}，本機 {len(local)} B"
            failed += 1
        print(f"  ok   {target:<36} {size_txt}"
              f"{'  sha256=' + sha[:12] + '…' if sha else ''}{note}")
    print(f"上傳完成：{len(file_list(src)) - failed}/{len(file_list(src))} 個檔 OK")
    return failed


def reboot(via: str, timeout: float) -> int:
    print("\n重開裝置（POST /api/reboot）")
    try:
        status, raw = _http(via, "POST", "/api/reboot", b"", timeout)
    except UploadError as e:
        print(f"  FAIL {e}")
        return 1
    if status >= 300:
        print(f"  FAIL HTTP {status}：{raw[:300]!r}")
        return 1
    print("  ok   裝置收到重開指令（等 1～2 秒 reconnect）")
    return 0


# ═══════════════════════ 入口 ═══════════════════════
def main() -> int:
    ap = argparse.ArgumentParser(
        description="上傳 8×8 Show 的 /pixel 內容（effects/map/modes/registry）",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="預設來源：ports/project/S3/ESP32-S3-Matrix-Show/data/pixel",
    )
    src_grp = ap.add_mutually_exclusive_group()
    src_grp.add_argument("--via", metavar="URL",
                         help=f"WebMaster REST（預設 {DEFAULT_VIA}）")
    src_grp.add_argument("--port", metavar="PORT",
                         help="直接開 CDC（暫未支援；請用 --via）")
    ap.add_argument("--base", default=DEFAULT_BASE,
                    help=f"裝置目標目錄（預設 {DEFAULT_BASE}）")
    ap.add_argument("--src", default=str(DEFAULT_SRC),
                    help="本機來源目錄（預設 project 的 data/pixel）")
    ap.add_argument("--check", action="store_true", help="只做本機 JSON 檢查，不上傳")
    ap.add_argument("--reboot", action="store_true", help="上傳完 POST /api/reboot")
    ap.add_argument("--timeout", type=float, default=30.0, help="每個 HTTP 請求 timeout 秒")
    args = ap.parse_args()

    if args.port is not None and not args.check:
        print("--port 未支援：直接開 CDC 寫檔要自己處理 NC4 FILE_* 命令同 FAT ownership。\n"
              f"請用 --via（預設 {DEFAULT_VIA}）經 WebMaster 的同一條 CDC。",
              file=sys.stderr)
        return 2

    base = args.base.strip().rstrip("/")
    if base and not base.startswith("/"):
        base = "/" + base
    if ".." in base.split("/"):
        print(f"--base 唔可以含 '..'：{args.base!r}", file=sys.stderr)
        return 2

    src = Path(args.src).expanduser().resolve()
    print("show_upload.py — 8×8 Show /pixel 內容（低亮度 breathe／white mix）")
    print(f"來源：{src}")
    reports, errs = check_tree(src)
    print("\n本機 JSON 檢查：")
    if not _print_checks(reports, errs):
        print("\n檢查唔過：唔會上傳。", file=sys.stderr)
        return 1
    if args.check:
        return 0

    print()
    failed = upload_files(args.via or DEFAULT_VIA, base, src, args.timeout)
    if args.reboot and failed == 0:
        failed += reboot(args.via or DEFAULT_VIA, args.timeout)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
