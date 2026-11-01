"""路徑、IDF 解析、子行程。所有模組都從這裡拿共用的東西。"""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent.parent      # tools/
ROOT = TOOLS.parent                                  # repo 根
FIRMWARE = ROOT / "firmware"
ARTIFACTS_NAME = "artifacts"

# 讓 `import board` 找得到 tools/board.py（板子 metadata 的真相）
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))


# ───────────────────────── 設定 ─────────────────────────

DEFAULT_CONFIG = {
    "default_board": None,
    "build": {
        "jobs": None,          # None = idf.py 自己決定（nproc）
        "archive": True,       # 產物複製到 artifacts/ 並帶時間戳
        "doctor": True,        # build 前跑環境健檢（快檢，不是全套）
        "output_dir": ARTIFACTS_NAME,
        "keep": 10,            # 每塊板子保留幾個歷史產物
    },
    "idf": {
        "path": None,          # 沒設就 ~/esp/esp-idf
        "per_chip": {},        # 例如 {"P4": "~/esp/esp-idf-p4"}
    },
    "serial": {"port": None, "baud": 115200},
}


def load_config(explicit: str | None = None) -> dict:
    """讀 make_config.json（或 --config 指定的），缺的鍵用預設補。"""
    path = None
    if explicit:
        path = Path(explicit).expanduser().resolve()
        if not path.exists():
            raise SystemExit(f"找不到設定檔：{path}")
    else:
        for name in ("make_config.json", "ncbuild.json", "make_config.example.json"):
            cand = ROOT / name
            if cand.exists():
                path = cand
                break

    cfg = json.loads(json.dumps(DEFAULT_CONFIG))  # deep copy
    if path and path.exists():
        try:
            user = json.loads(path.read_text())
        except json.JSONDecodeError as e:
            raise SystemExit(f"{path.name} 不是合法 JSON：{e}")
        for k, v in user.items():
            if isinstance(v, dict) and isinstance(cfg.get(k), dict):
                cfg[k].update(v)
            else:
                cfg[k] = v
        cfg["_path"] = str(path)
    return cfg


def artifacts_dir(cfg: dict) -> Path:
    d = Path(cfg.get("build", {}).get("output_dir") or ARTIFACTS_NAME).expanduser()
    return d if d.is_absolute() else ROOT / d


# ───────────────────────── 板子 ─────────────────────────

def resolve_board(query: str | None) -> tuple[Path, dict, str, str]:
    """回傳 (port 目錄, board.json, idf target, 晶片目錄名如 P4)。"""
    import board as boardmod   # tools/board.py

    if not query:
        query = None
    try:
        dirs = boardmod.find_boards(query) if query else boardmod.board_dirs()
    except SystemExit as e:
        raise SystemExit(str(e))
    if not dirs:
        raise SystemExit("ports/ 下沒有任何 board.json —— 先 python3 tools/board.py new …")
    d = dirs[0]
    b = boardmod.load_board(d)
    chip = b["chip"]
    return d, b, chip, d.parent.name.upper()


def port_name(d: Path) -> str:
    """傳給 CMake 的 -DPORT（相對於 repo 根的 ports/）。"""
    return f"{d.parent.name}/{d.name}"


# ───────────────────────── IDF ─────────────────────────

def idf_path_for(chip: str, cfg: dict) -> Path:
    per = (cfg.get("idf") or {}).get("per_chip") or {}
    candidates = [
        os.environ.get(f"IDF_{chip.upper()}"),
        per.get(chip.upper()) or per.get(chip),
        os.environ.get("IDF_PATH"),
        (cfg.get("idf") or {}).get("path"),
        str(Path.home() / "esp" / "esp-idf"),
    ]
    for c in candidates:
        if c:
            return Path(str(c)).expanduser()
    return Path.home() / "esp" / "esp-idf"


def idf_version(idf: Path) -> str | None:
    """version.txt 在新版 IDF 可能不存在（純 git clone），退回 git describe。"""
    vf = idf / "version.txt"
    if vf.exists():
        return vf.read_text().strip()
    if (idf / ".git").exists():
        rc, out = bash_capture("git describe --tags --always", cwd=idf)
        if rc == 0 and out:
            return out.strip()
    return None


def export_prefix(idf: Path) -> str:
    return f'. "{idf}/export.sh" >/dev/null 2>&1'


def bash(cmd: str, cwd: Path | None = None, log: Path | None = None,
         echo: bool = True) -> int:
    """跑 bash -c；有 log 就同時寫檔。回傳 exit code。"""
    p = subprocess.Popen(
        ["bash", "-c", cmd],
        cwd=str(cwd) if cwd else None,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    fh = None
    if log:
        log.parent.mkdir(parents=True, exist_ok=True)
        fh = log.open("w", encoding="utf-8")
        fh.write(f"# {cmd}\n\n")
    try:
        assert p.stdout is not None
        for line in p.stdout:
            if echo:
                sys.stdout.write(line)
                sys.stdout.flush()
            if fh:
                fh.write(line)
    finally:
        if fh:
            fh.close()
    return p.wait()


def bash_capture(cmd: str, cwd: Path | None = None, timeout: int = 60) -> tuple[int, str]:
    try:
        r = subprocess.run(["bash", "-c", cmd], cwd=str(cwd) if cwd else None,
                           capture_output=True, text=True, timeout=timeout)
        return r.returncode, (r.stdout + r.stderr).strip()
    except subprocess.TimeoutExpired:
        return 124, "timeout"


# ───────────────────────── 雜項 ─────────────────────────

def human(n: int | float) -> str:
    for unit in ("B", "KB", "MB", "GB"):
        if abs(n) < 1024 or unit == "GB":
            return f"{n:.1f} {unit}" if unit != "B" else f"{int(n)} B"
        n /= 1024
    return f"{n:.1f} GB"


def git_rev() -> str:
    rc, out = bash_capture("git rev-parse --short HEAD", cwd=ROOT)
    return out if rc == 0 else "unknown"


def git_dirty() -> bool:
    rc, out = bash_capture("git status --porcelain", cwd=ROOT)
    return rc == 0 and bool(out)


def serial_ports() -> list[str]:
    """macOS 的 USB 序列埠。Linux 也順便支援。"""
    out: list[str] = []
    for pat in ("/dev/cu.usbmodem*", "/dev/tty.usbmodem*", "/dev/ttyACM*", "/dev/ttyUSB*"):
        out.extend(str(p) for p in sorted(Path("/").glob(pat.lstrip("/"))))
    seen, uniq = set(), []
    for p in out:
        if p not in seen:
            seen.add(p)
            uniq.append(p)
    return uniq


def pick_port(cfg: dict, explicit: str | None) -> str | None:
    if explicit:
        return explicit
    cfgp = (cfg.get("serial") or {}).get("port")
    if cfgp:
        return cfgp
    ports = serial_ports()
    return ports[0] if ports else None


def which(name: str) -> str | None:
    return shutil.which(name)
