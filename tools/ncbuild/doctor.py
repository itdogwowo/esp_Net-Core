"""環境健檢：在 build 之前把「會讓你卡半小時」的東西先抓出來。"""
from __future__ import annotations

import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

from . import env

OK, WARN, FAIL = "ok", "warn", "fail"
SYM = {OK: "✔", WARN: "△", FAIL: "✘"}


@dataclass
class Check:
    name: str
    status: str
    detail: str
    hint: str = ""


def _board_check() -> Check:
    rc, out = env.bash_capture(f'"{sys.executable}" "{env.TOOLS / "board.py"}" check',
                              cwd=env.ROOT, timeout=120)
    if rc == 0:
        tail = out.strip().splitlines()[-1] if out.strip() else ""
        return Check("板子設定", OK, tail or "全部通過")
    first = [ln for ln in out.splitlines() if "ERROR" in ln]
    return Check("板子設定", FAIL, f"{len(first)} 個錯誤",
                 "跑 python3 tools/board.py check 看細節")


def _sdkconfig_freshness() -> Check:
    gen = env.FIRMWARE / "sdkconfig"
    if not gen.exists():
        return Check("sdkconfig", OK, "還沒有（第一次 build 會產生）")
    defaults = [env.FIRMWARE / "sdkconfig.defaults"]
    defaults += [d / "sdkconfig.defaults" for d in (env.ROOT / "ports").glob("*/*/")]
    defaults = [p for p in defaults if p.exists()]
    if not defaults:
        return Check("sdkconfig", OK, "沒有 defaults 可比較")
    newest = max(p.stat().st_mtime for p in defaults)
    if gen.stat().st_mtime < newest:
        return Check("sdkconfig", WARN, "比 sdkconfig.defaults 舊",
                     "改了不會生效：rm firmware/sdkconfig 再 build（make.py build --clean 也行）")
    return Check("sdkconfig", OK, "與 defaults 同步")


def run_checks(cfg: dict, board_query: str | None = None, quick: bool = False) -> list[Check]:
    checks: list[Check] = []

    # 1) Python
    v = sys.version_info
    checks.append(Check("Python", OK if v >= (3, 9) else FAIL,
                        f"{v.major}.{v.minor}.{v.micro}",
                        "" if v >= (3, 9) else "需要 3.9+"))

    # 2) 選一塊板子當代表（決定要檢查哪個 IDF）
    try:
        d, b, chip, chip_dir = env.resolve_board(board_query, require=False)
        board_label = f"{b.get('name', d.name)} ({chip})"
    except SystemExit as e:
        checks.append(Check("板子", FAIL, str(e), "python3 tools/board.py new …"))
        d = b = None
        chip, chip_dir, board_label = "esp32s3", "S3", "?"

    idf = env.idf_path_for(chip, cfg)

    # 3) IDF 目錄
    if (idf / "export.sh").exists():
        checks.append(Check("ESP-IDF", OK, f"{idf} ({env.idf_version(idf) or '版本未知'})"))
    else:
        checks.append(Check("ESP-IDF", FAIL, f"找不到 {idf}",
                            "設定 IDF_PATH，或 make_config.json 的 idf.path / idf.per_chip"))

    # 4/5) idf.py 與 esptool（要 source export.sh，慢一點）
    if not quick and (idf / "export.sh").exists():
        rc, out = env.bash_capture(f'{env.export_prefix(idf)} && idf.py --version', timeout=120)
        checks.append(Check("idf.py", OK if rc == 0 else FAIL,
                            out.splitlines()[-1] if out else "無輸出",
                            "" if rc == 0 else "source export.sh 失敗；看 docs/02_guides/build_and_flash.md"))
        rc, out = env.bash_capture(f'{env.export_prefix(idf)} && python -m esptool version', timeout=120)
        ver = next((ln for ln in out.splitlines() if ln.strip().startswith("esptool")), out[:60])
        checks.append(Check("esptool", OK if rc == 0 else FAIL, ver or "無輸出",
                            "" if rc == 0 else "IDF 的 python env 裡沒有 esptool"))
    else:
        checks.append(Check("idf.py / esptool", WARN, "已跳過（--quick）"))

    # 6) 主機端工具需要的 pyserial
    rc, out = env.bash_capture(f'"{sys.executable}" -c "import serial; print(serial.__version__)"', timeout=30)
    checks.append(Check("pyserial", OK if rc == 0 else WARN, out or "未安裝",
                        "" if rc == 0 else "tools/*.py 需要：pip3 install pyserial"))

    # 7) 序列埠（資訊性）
    ports = env.serial_ports()
    checks.append(Check("序列埠", OK if ports else WARN,
                        ", ".join(ports) if ports else "沒有偵測到 USB 序列埠",
                        "" if ports else "只有 flash / monitor 需要；build 不需要"))

    # 8) 板子設定
    if d is not None:
        checks.append(_board_check())

    # 9) sdkconfig 新鮮度
    checks.append(_sdkconfig_freshness())

    # 10) 磁碟
    du = shutil.disk_usage(env.ROOT)
    free_gb = du.free / (1024 ** 3)
    checks.append(Check("磁碟", OK if free_gb >= 2 else WARN,
                        f"{free_gb:.1f} GB 可用（build 約 0.3 GB）",
                        "" if free_gb >= 2 else "清掉 firmware/build 再試"))

    # 11) git
    dirty = env.git_dirty()
    checks.append(Check("git", WARN if dirty else OK,
                        f"{env.git_rev()}{'（有未提交變更）' if dirty else ''}",
                        "" if not dirty else "產物會標記 git_dirty；出貨前記得 commit"))

    # 12) 產物目錄
    out_dir = env.artifacts_dir(cfg)
    checks.append(Check("產物目錄", OK, str(out_dir.relative_to(env.ROOT))
                        if out_dir.is_relative_to(env.ROOT) else str(out_dir)))

    return checks


def report(checks: list[Check], title: str = "環境健檢") -> int:
    print(f"🩺 {title}")
    print("─" * 72)
    for c in checks:
        print(f"{SYM[c.status]} {c.name:<12} {c.detail}")
        if c.hint and c.status != OK:
            print(f"  {'':<12} → {c.hint}")
    fails = sum(1 for c in checks if c.status == FAIL)
    warns = sum(1 for c in checks if c.status == WARN)
    print("─" * 72)
    if fails:
        print(f"{fails} 個問題必須先修（{warns} 個警告）")
    elif warns:
        print(f"可以建置（{warns} 個警告）")
    else:
        print("全部正常")
    return 1 if fails else 0
