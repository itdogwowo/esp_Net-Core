#!/usr/bin/env python3
"""ESPTOOL（ROM／USB-Serial-JTAG）燒錄：server 暫放序列埠，跑 esptool，再重新連線。

為什麼不是瀏覽器 Web Serial（esptool-js）：
  序列埠只可以有一個擁有者。這個工作臺的價值是「UI 與 AI 同時用同一條線」，
  所以燒錄都經 server；UI 只是下指令。功能等同 esptool-js，但多 client 安全。
"""
from __future__ import annotations

import re
import subprocess
import sys
from typing import Callable

PCT_RE = re.compile(r"\((\d+)\s*%\)")


class FlashError(RuntimeError):
    pass


def build_cmd(port: str, image_path: str | None, *, offset: int = 0,
              baud: int = 460800, chip: str = "", before: str = "usb_reset",
              erase: bool = False) -> list[str]:
    cmd = [sys.executable, "-m", "esptool", "--port", port, "--baud", str(baud)]
    if chip:
        cmd += ["--chip", chip]
    cmd += ["--before", before, "--after", "hard_reset", "--no-stub"]
    if erase:
        cmd.append("erase_flash")
    elif image_path:
        cmd += ["write_flash", hex(int(offset)), image_path]
    else:
        raise FlashError("冇 image 又冇 erase")
    return cmd


def build_info_cmd(port: str, *, chip: str = "", before: str = "usb_reset") -> list[str]:
    cmd = [sys.executable, "-m", "esptool", "--port", port]
    if chip:
        cmd += ["--chip", chip]
    cmd += ["--before", before, "--after", "hard_reset", "--no-stub", "flash_id"]
    return cmd


def run_flash(cmd: list[str], *, timeout: float = 240.0,
              on_line: Callable[[str], None] | None = None,
              on_progress: Callable[[int], None] | None = None) -> int:
    """行 esptool，逐行 callback；回傳 exit code。"""
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, bufsize=1)
    assert proc.stdout is not None
    try:
        for line in proc.stdout:
            line = line.rstrip("\r\n")
            if not line:
                continue
            if on_line:
                on_line(line)
            m = PCT_RE.search(line)
            if m and on_progress:
                on_progress(int(m.group(1)))
        return proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        raise FlashError(f"esptool timeout（>{timeout:.0f}s）")
    finally:
        try:
            proc.stdout.close()
        except Exception:
            pass
