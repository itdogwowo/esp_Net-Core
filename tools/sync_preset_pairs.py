#!/usr/bin/env python3
"""中央示範 preset ↔ 硬件＋project pair 同步檢查（監察「最新情況」）。

    python3 tools/sync_preset_pairs.py --check    # run_all 用：有 drift 就 fail
    python3 tools/sync_preset_pairs.py --write    # 由 preset 重寫 project 的 runtime 檔

背景：`ports/presets/s3-generic-*` 係中央基準，但 project 的 data/config 有時只更新
一部分。呢個工具把「兩個示範 pair」釘死：
  · project 的 system／network／tasks／resources 一定同 preset 一樣
  · board／project 的 LED 幾何一定同 preset driver.json 一樣
  · project 的 I80／USB 接線（由 wiring 生成）同 preset hardware.json 對得上

pair：
  s3-generic-psram  ↔ S3/ESP32-S3-Generic-PSRAM    ↔ project 同名
  s3-generic-nopsram ↔ S3/ESP32-S3-Generic-NoPSRAM ↔ project 同名

注意：hardware.json／driver.json 的**完整**檔案由 wiring／board driver 生成
（`tools/sync_hardware_presets.py`／`tools/sync_driver_presets.py` 管），
所以呢度只比對關鍵欄位，唔會覆寫 hardware。
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PRESETS = ROOT / "ports/presets"
HW = ROOT / "ports/hardware"
PJ = ROOT / "ports/project"

PAIRS = [
    ("s3-generic-psram", "S3/ESP32-S3-Generic-PSRAM", "S3/ESP32-S3-Generic-PSRAM"),
    ("s3-generic-nopsram", "S3/ESP32-S3-Generic-NoPSRAM", "S3/ESP32-S3-Generic-NoPSRAM"),
]
RUNTIME_FILES = ["system", "network", "tasks", "resources"]


def load(p: Path) -> dict:
    return json.loads(p.read_text(encoding="utf-8"))


def strip_comments(v):
    if isinstance(v, dict):
        return {k: strip_comments(x) for k, x in v.items() if not k.startswith("_")}
    if isinstance(v, list):
        return [strip_comments(x) for x in v]
    return v


def led_cfg(doc: dict) -> dict | None:
    return doc.get("LED") if isinstance(doc, dict) else None


def check_pair(preset: str, hardware: str, project: str) -> list[str]:
    errs: list[str] = []
    pd = PRESETS / preset / "data/config"
    hw = HW / hardware
    pj = PJ / project
    pdocs = {f: load(pd / f"{f}.json") for f in ["driver", "hardware"] + RUNTIME_FILES}
    if not (hw / "driver.json").is_file():
        errs.append(f"{hardware}: 缺 driver.json")
        return errs
    hw_drv = load(hw / "driver.json")
    pj_docs = {}
    for f in ["driver", "hardware"] + RUNTIME_FILES:
        p = pj / "data/config" / f"{f}.json"
        if not p.is_file():
            errs.append(f"{project}: 缺 data/config/{f}.json")
            continue
        pj_docs[f] = load(p)

    # 1) runtime 檔：project 要同 preset 一樣
    for f in RUNTIME_FILES:
        if f not in pj_docs:
            continue
        if strip_comments(pj_docs[f]) != strip_comments(pdocs[f]):
            errs.append(f"{project}/{f}.json 同 preset {preset} 唔一致（跑 --write）")

    # 2) LED 幾何：board driver／project driver 要同 preset driver 一樣
    want_led = strip_comments(led_cfg(pdocs["driver"]) or {})
    for label, doc in [(f"{hardware}/driver.json", hw_drv), (f"{project}/driver.json", pj_docs.get("driver"))]:
        if doc is None:
            continue
        got = strip_comments(led_cfg(doc) or {})
        if got != want_led:
            errs.append(f"{label} 的 LED 段同 preset {preset} 唔一致")

    # 3) hardware 接線關鍵欄位：由 wiring 生成，但要同 preset 對得上
    want_hw = pdocs["hardware"]
    got_hw = pj_docs.get("hardware", {})
    for sec in ("I80", "USB"):
        w = want_hw.get(sec)
        g = got_hw.get(sec)
        if w is None:
            continue
        if (g or {}).get("enable") != w.get("enable"):
            errs.append(f"{project}/hardware.json {sec}.enable={ (g or {}).get('enable') } != {w.get('enable')}")
            continue
        if sec == "I80":
            wl = (w.get("list") or [{}])[0]
            gl = ((g or {}).get("list") or [{}])[0]
            if gl.get("data") != wl.get("data"):
                errs.append(f"{project}/hardware.json I80.data={gl.get('data')} != {wl.get('data')}")
        if sec == "USB":
            wl = (w.get("list") or [{}])
            gl = ((g or {}).get("list") or [{}])
            if [(x.get("phy"), x.get("enable")) for x in gl] != [(x.get("phy"), x.get("enable")) for x in wl]:
                errs.append(f"{project}/hardware.json USB.list 同 preset {preset} 唔一致")
    return errs


def do_write(preset: str, project: str) -> None:
    src = PRESETS / preset / "data/config"
    dst = PJ / project / "data/config"
    dst.mkdir(parents=True, exist_ok=True)
    for f in RUNTIME_FILES:
        (dst / f"{f}.json").write_bytes((src / f"{f}.json").read_bytes())


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true", help="由 preset 重寫 project 的 runtime 檔")
    ap.add_argument("--check", action="store_true", help="檢查 drift（run_all 用）")
    args = ap.parse_args()

    if args.write:
        for preset, _hw, project in PAIRS:
            do_write(preset, project)
            print(f"✔ {preset} → {project}/data/config（runtime 4 檔）")
        print("提醒：hardware／driver 由 wiring／board driver 生成，跑 sync_*_presets.py 更新。")

    errs: list[str] = []
    for preset, hw, project in PAIRS:
        errs.extend(check_pair(preset, hw, project))
    for e in errs:
        print(f"✘ {e}", file=sys.stderr)
    if errs:
        print(f"—— {len(errs)} 個 drift", file=sys.stderr)
        return 1
    if not args.write:
        print(f"preset_pair OK：{len(PAIRS)} 對（preset ↔ hardware ↔ project）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
