"""路徑、IDF 解析、子行程。所有模組都從這裡拿共用的東西。"""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
from dataclasses import dataclass
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
        "build_dir": None,     # None = <output_dir>/build（每塊板一個子目錄）
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
            user = json.loads(path.read_text(encoding="utf-8"))
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


def build_root(cfg: dict) -> Path:
    """所有 CMake/IDF 中間檔的統一父目錄。

    預設 <artifacts>/build；`build.build_dir` 可以指到專案外（例如
    `~/.cache/esp_Net-Core/build`），專案樹就不會有 build_p4、build_p4c6。
    """
    raw = (cfg.get("build") or {}).get("build_dir")
    if raw:
        d = Path(str(raw)).expanduser()
        return d if d.is_absolute() else ROOT / d
    return artifacts_dir(cfg) / "build"


def build_slug(port: "Port") -> str:
    """一個「板 × project」一個 build dir：<chip>_<output_name>。

    default project 用番 <chip>_<board>（同以前一樣）；非 default project 加
    __<project 目錄名>，避免同一個 build dir 互相洗掉 cache／sdkconfig。
    """
    return f"{port.hw.parent.name}_{port.output_name}"


def build_dir(cfg: dict, port: "Port") -> Path:
    return build_root(cfg) / build_slug(port)


def sdkconfig_path(cfg: dict, port: "Port") -> Path:
    return build_dir(cfg, port) / "sdkconfig"


# ───────────────────────── 板子 ─────────────────────────

@dataclass(frozen=True)
class Port:
    """一次建置的兩層身分：硬體板（必）＋ project（選）。"""
    hw: Path
    pj: Path | None
    board: dict
    chip: str
    default_pj: Path | None = None   # 這塊板的 default project（用來判斷 output 名）

    @property
    def hw_rel(self) -> str:
        return f"{self.hw.parent.name}/{self.hw.name}"

    @property
    def pj_rel(self) -> str:
        return "" if self.pj is None else f"{self.pj.parent.name}/{self.pj.name}"

    @property
    def name(self) -> str:
        return self.board.get("name", self.hw.name)

    @property
    def output_name(self) -> str:
        """artifacts／build dir／log 用的名字。

        - default project（或冇 project）：<硬體板>（同以前一樣）
        - `--no-project` 但這塊板有 default：<硬體板>__bare（唔會蓋走 default 的 image）
        - 指定非 default project：<硬體板>__<project 目錄名>
        """
        base = self.name
        if self.pj is None:
            return base if self.default_pj is None else f"{base}__bare"
        if self.default_pj is not None and self.pj == self.default_pj:
            return base
        return f"{base}__{self.pj.name}"


def resolve_board(query: str | None, *, project: str | None = None,
                  no_project: bool = False, require: bool = True) -> Port:
    """回傳硬體板 + project 的身分。

    查詢可以命中任一層：硬體板名、project 名、<chip>/<board>。
    沒有 project 參數 → 用這塊板的 default project（同名的或 default:true 的）；
    連 default 都沒有 → 純硬體（冇 project 預置）。

    project  = 指定 project（短名可：`--project demo` 會中 <硬體板>-Demo）
    no_project = 明確唔要 project（就算有 default 都用純硬體）
    """
    import board as boardmod   # tools/board.py

    try:
        pairs = boardmod.find_ports(query) if query else boardmod.find_ports(None)
    except SystemExit as e:
        raise SystemExit(str(e))
    if not pairs:
        raise SystemExit("ports/ 下沒有任何 board.json —— 先 python3 tools/board.py new …")
    if query is None and require:
        names = "、".join(boardmod.load_board(d).get("name", d.name) for d, _ in pairs)
        raise SystemExit(
            f"沒有指定板子（目前有：{names}）\n"
            f"  → python3 make.py build <硬體板> [--project <project>]\n"
            f"  → 常用的那塊可寫進 make_config.json 的 default_board"
        )
    d, p = pairs[0]
    b = boardmod.load_board(d)
    default_pj = boardmod.default_project_for_hardware(d)
    chosen: Path | None = default_pj

    if project is not None:
        if p is not None and default_pj is not None and p != default_pj:
            raise SystemExit(f"查詢 {query!r} 已經係 project {p.name!r}，"
                             f"唔可以同時再加 --project {project!r}")
        hit = boardmod.find_project_for_hardware(d, project)
        if hit is None:
            avail = boardmod.projects_for_hardware(d)
            hint = ("；有：" + "、".join(
                x.name + ("（default）" if x == default_pj else "") for x in avail)
                if avail else "（一個 project 都冇）")
            raise SystemExit(f"{b.get('name', d.name)} 冇 project {project!r}{hint}")
        chosen = hit
    elif no_project:
        chosen = None
    elif p is not None and default_pj is not None and p != default_pj:
        chosen = p          # 查詢本身就係非 default project
    else:
        chosen = default_pj
    return Port(hw=d, pj=chosen, board=b, chip=b["chip"], default_pj=default_pj)


def port_name(d: Path) -> str:
    """傳給 CMake 的 -DPORT（相對於 ports/hardware/）。"""
    return f"{d.parent.name}/{d.name}"


def project_name(d: Path | None) -> str:
    """傳給 CMake 的 -DPROJECT（相對於 ports/project/）；空字串 = 沒有 project。"""
    return "" if d is None else f"{d.parent.name}/{d.name}"


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
