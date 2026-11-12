#!/usr/bin/env python3
"""救磚／重刷：自動偵測晶片＋flash size，揀 artifacts/recovery/ 對應映像。

用法（板子要先入 download mode）：
    # 1) 按住 BOOT，tap RESET，放開 BOOT
    # 2) 睇住個新 COM port（通常係 303A:1001 或 USB Serial）
    python recover.py --port COM25
    python recover.py --port COM25 --dry-run     # 只印命令

支援：
    ESP32-P4 + 16MB → ESP32-P4-C6
    ESP32-P4 + 32MB → ESP32-P4-ETH
    ESP32-S3 + 16MB → ESP32-S3-N16R8
    ESP32-S3 +  4MB → ESP32-S3-Matrix
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

from serial.tools import list_ports

try:                                    # Windows cp950 console 唔識 ✘/✔
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

ROOT = Path(__file__).resolve().parents[2]
REC = ROOT / "artifacts" / "recovery"

# (chip prefix, flash MB) -> (recovery dir, bootloader offset, flash freq)
MAP = {
    ("ESP32-P4", 16): ("ESP32-P4-C6", 0x2000, "40m"),
    ("ESP32-P4", 32): ("ESP32-P4-ETH", 0x2000, "40m"),
    ("ESP32-S3", 16): ("ESP32-S3-N16R8", 0x0000, "80m"),
    ("ESP32-S3", 4): ("ESP32-S3-Matrix", 0x0000, "80m"),
}


def _ports() -> list[str]:
    return [p.device for p in list_ports.comports()]


def _run(cmd: list[str]) -> subprocess.CompletedProcess:
    print("$", " ".join(cmd))
    return subprocess.run(cmd, capture_output=True, text=True)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None, help="例如 COM25 / /dev/ttyACM0")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    port = args.port
    if not port:
        ports = _ports()
        print("可用 ports:", ports)
        port = input("Port: ").strip()
    if not port:
        return 2

    # 先問晶片同 flash size（esptool 會 auto-detect）。
    # --after no_reset：flash_id 預設會 RTS reset，會即刻離開 ROM mode。
    r = _run([sys.executable, "-m", "esptool", "-p", port,
              "--before", "no_reset", "--after", "no_reset", "flash_id"])
    out = (r.stdout or "") + (r.stderr or "")
    print(out)
    if r.returncode != 0:
        print("\n[X] esptool 連唔到。請確認：")
        print("  - 板子已入 download mode（按住 BOOT -> tap RESET -> 放開 BOOT）")
        print("  - port 正確、冇其他程式佔用")
        return 1

    m_chip = re.search(r"(?:Chip type:|Connected to)\s+(ESP32-[A-Z0-9]+)", out)
    m_size = re.search(r"Detected flash size:\s*(\d+)MB", out)
    if not m_chip or not m_size:
        print("\n[X] 睇唔出 chip / flash size；請貼上面輸出。")
        return 1
    chip = m_chip.group(1)
    size = int(m_size.group(1))
    print(f"\n偵測到：{chip} / {size} MB")

    key = (chip, size)
    if key not in MAP:
        print(f"[X] 冇 {key} 的 recovery 映像；請 build 對應 board 再 copy 去 {REC}")
        return 1
    board, offset, freq = MAP[key]
    d = REC / board
    files = ["bootloader.bin", "partition-table.bin", "ota_data_initial.bin", "esp_net_core.bin"]
    missing = [f for f in files if not (d / f).is_file()]
    if missing:
        print(f"[X] {d} 缺：{missing}")
        return 1

    cmd = [
        sys.executable, "-m", "esptool", "-p", port, "-b", "460800",
        "write-flash", "--flash-mode", "dio", "--flash-freq", freq,
        "--flash-size", f"{size}MB",
        hex(offset), str(d / "bootloader.bin"),
        "0x8000", str(d / "partition-table.bin"),
        "0xf000", str(d / "ota_data_initial.bin"),
        "0x20000", str(d / "esp_net_core.bin"),
    ]
    print(f"\n將會燒 {board}（bootloader @ {hex(offset)}）")
    if args.dry_run:
        print("$", " ".join(cmd))
        return 0
    if input("確認？(y/N) ").strip().lower() != "y":
        return 1

    r = _run(cmd)
    print(r.stdout)
    print(r.stderr)
    if r.returncode != 0:
        print("[X] 燒錄失敗")
        return 1
    print("\n[OK] 完成。拔插 USB，等板子重啟；再開 UI 連 CDC。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
