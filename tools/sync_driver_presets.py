#!/usr/bin/env python3
"""把 driver.json 的 devices 預置鏡像同步到 project user-space（同 hardware.json 對稱）。

為什麼要有這個檔：
  driver.json（build-time 板級）只有 firmware 睇；用戶插上 USB 後想喺 /data/config
  見到一份**具體、可改**的 devices.json（runtime device 層）。
  ncportgen 會產生它；這裡把它鏡像到 ports/project/<chip>/<board>/data/config/devices.json
  （同 hardware.json 一樣的做法），build 時 CMake 優先嵌入 project 版。

用法：
  python3 tools/sync_devices_presets.py            # 全部有 driver.json 的板（default project）
  python3 tools/sync_devices_presets.py S3         # 只做 chip=S3
  python3 tools/sync_devices_presets.py --check    # 只檢查，不寫檔
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HW = ROOT / "ports" / "hardware"
GEN = ROOT / "tools" / "ncportgen.py"

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(errors="replace")


def boards(chip_filter: str | None):
    import board as boardmod

    for hw in sorted(HW.glob("*/*/driver.json")):
        chip, board = hw.parts[-3], hw.parts[-2]
        if chip.startswith("_"):
            continue
        if chip_filter and chip.lower() != chip_filter.lower():
            continue
        proj = boardmod.default_project_for_hardware(hw.parent)
        if proj is not None and proj.is_dir():
            yield chip, board, hw.parent, proj


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("chip", nargs="?", help="只做這個 chip（例如 P4／S3）")
    ap.add_argument("--check", action="store_true", help="只檢查，不寫檔")
    args = ap.parse_args()

    changed = unlike = 0
    for chip, board, hwdir, proj in boards(args.chip):
        with tempfile.TemporaryDirectory() as td:
            r = subprocess.run([sys.executable, str(GEN), "--hardware", str(hwdir), "--out", td],
                               capture_output=True, text=True, encoding="utf-8", errors="replace")
            if r.returncode != 0:
                unlike += 1
                print(f"✘ {chip}/{board}: ncportgen 失敗：{(r.stderr or '').strip()[:160]}")
                continue
            src = Path(td) / "preset_devices.json"
            if not src.is_file():
                unlike += 1
                print(f"✘ {chip}/{board}: 冇 preset_devices.json")
                continue
            want = json.loads(src.read_text(encoding="utf-8"))
            dst = proj / "data" / "config" / "devices.json"
            have = json.loads(dst.read_text(encoding="utf-8")) if dst.is_file() else None
            if have == want:
                print(f"  = {proj.relative_to(ROOT)}/data/config/devices.json（已同步）")
                continue
            if args.check:
                unlike += 1
                print(f"  ≠ {proj.relative_to(ROOT)}/data/config/devices.json 需要同步")
                continue
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_text(json.dumps(want, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
            changed += 1
            print(f"  ✔ {dst.relative_to(ROOT)}（{len(json.dumps(want))} 字元）")

    if args.check:
        print(f"\n--check：{unlike} 個需要同步")
        return 1 if unlike else 0
    print(f"\n完成：更新 {changed} 個，失敗 {unlike} 個")
    return 0 if unlike == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
