#!/usr/bin/env python3
"""把 hardware/wiring.json 的完整 hardware.json 預置同步到 project user-space。

為什麼要有這個檔：
  firmware 的開機預設（nc_port.c）由 wiring.json 生成，但用戶插上 USB 後想看到
  一份**具體、可改**的 /data/config/hardware.json。這份檔案由 ncportgen 產生，
  放在 ports/project/<chip>/<board>/data/config/hardware.json；build 時優先嵌入它。

用法：
  python3 tools/sync_hardware_presets.py           # 全部有 project 的板
  python3 tools/sync_hardware_presets.py P4        # 只做 chip=P4 的板
  python3 tools/sync_hardware_presets.py --check    # 只檢查有沒有不同步
"""
from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HW = ROOT / "ports" / "hardware"
PJ = ROOT / "ports" / "project"
GEN = ROOT / "tools" / "ncportgen.py"


def boards(chip_filter: str | None):
    for hw in sorted(HW.glob("*/*/wiring.json")):
        chip, board = hw.parts[-3], hw.parts[-2]
        if chip.startswith("_"):
            continue
        if chip_filter and chip.lower() != chip_filter.lower():
            continue
        proj = PJ / chip / board
        if proj.is_dir():
            yield chip, board, hw.parent, proj


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("chip", nargs="?", help="只做這個 chip（例如 P4／S3）")
    ap.add_argument("--check", action="store_true", help="只檢查，不寫檔")
    args = ap.parse_args()

    changed = 0
    for chip, board, hwdir, proj in boards(args.chip):
        with tempfile.TemporaryDirectory() as td:
            r = subprocess.run(
                [sys.executable, str(GEN), "--hardware", str(hwdir), "--out", td],
                capture_output=True, text=True)
            if r.returncode != 0:
                print(f"✘ {chip}/{board}: ncportgen failed\n{r.stderr}")
                return 1
            src = Path(td) / "preset_hardware.json"
            dst = proj / "data" / "config" / "hardware.json"
            new = src.read_text()
            old = dst.read_text() if dst.exists() else None
            if old == new:
                print(f"✔ {chip}/{board}: 已同步")
                continue
            changed += 1
            if args.check:
                print(f"△ {chip}/{board}: 不同步（跑 tools/sync_hardware_presets.py）")
            else:
                dst.write_text(new)
                print(f"↻ {chip}/{board}: 已寫 {dst.relative_to(ROOT)}")

    if args.check and changed:
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
