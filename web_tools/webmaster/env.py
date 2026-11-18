#!/usr/bin/env python3
"""環境自檢與安裝（doctor＋installer，跨平台）。

只回答三件事：
1. **而家缺咩**：Python／deps／esptool／git／cmake／ninja／node／ESP-IDF／工具鏈。
2. **每項缺咗有咩後果**：例如冇 esptool → 只可以用 NC4 OTA 或頁面燒錄；
   冇 IDF → 燒得到但**建置唔到** firmware。
3. **點補**：`install()` 會跑官方指令（git clone esp-idf + install.ps1／install.sh），
   輸出逐行回傳畀 UI／AI。**唔會自己偷偷裝**——一定要人／AI 明確要求。

設計取捨
--------
- 全部用「跑一支命令、睇 exit code／輸出」而唔去猜檔案路徑：
  版本號一律問工具本身（`git --version`、`idf.py --version`）。
- 探測**只讀**，寫入路徑（clone／pip install）全部集中在 `install()`。
- 唔假設任何安裝位置：先睇 `IDF_PATH`，再睇幾個慣例目錄（`~/esp/esp-idf`、
  `C:\\Espressif\\frameworks\\*`），最後睇 PATH 上的 `idf.py`。
"""
from __future__ import annotations

import os
import platform
import re
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Callable

BASE = Path(__file__).resolve().parent
TOOL_ROOT = BASE.parent

# 每個工具：id → (中文標籤, 缺咗有咩後果)
TOOL_INFO: dict[str, tuple[str, str]] = {
    "python": ("Python", "server 本身"),
    "pip": ("pip", "裝 deps／esptool"),
    "deps": ("Web deps（fastapi/uvicorn/pyserial）", "server 開唔到"),
    "esptool": ("esptool（Python）", "唔可以用 server 側 ROM 燒錄（NC4 OTA 不受影響）"),
    "git": ("Git", "唔可以 clone ESP-IDF／更新 repo"),
    "cmake": ("CMake", "建置 firmware（IDF 需要 ≥3.16）"),
    "ninja": ("Ninja", "建置 firmware（比 make 快）"),
    "node": ("Node.js", "只影響前端測試／重新打包 esptool-js"),
    "idf": ("ESP-IDF", "**建置** firmware（燒錄唔需要）"),
    "idf_toolchain": ("IDF 工具鏈（xtensa/riscv gcc）", "編譯目標晶片的程式碼"),
    "serial_driver": ("USB 序列（pyserial 可用）", "連唔到板子"),
}

_TIMEOUT = 20.0


def _run(cmd: list[str], timeout: float = _TIMEOUT) -> tuple[int, str]:
    """跑一支命令，回 (returncode, 合併輸出)。唔會 raise。

    強制 UTF-8：IDF 的 idf.py 喺 cp950 會印
    「Your environment is not configured to handle Unicode characters」而唔出結果。
    """
    try:
        env2 = dict(os.environ)
        env2.setdefault("PYTHONIOENCODING", "utf-8")
        env2.setdefault("PYTHONUTF8", "1")
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout,
                           errors="replace", env=env2)
        out = (p.stdout or "") + (p.stderr or "")
        return p.returncode, out.strip()
    except (OSError, subprocess.SubprocessError) as e:
        return -1, str(e)


def _first_line(text: str, limit: int = 120) -> str:
    for line in text.splitlines():
        line = line.strip()
        if line:
            return line[:limit]
    return ""


def _tool(cmd: str) -> str | None:
    return shutil.which(cmd)


def _is_windows() -> bool:
    """獨立成函式：測試可以 patch 佢，而唔需要改 os.name（會搞壞 pathlib）。"""
    return os.name == "nt"


# ── 各項探測 ───────────────────────────────────────────────────────────

def _check_python() -> dict:
    return {
        "id": "python", "status": "ok",
        "version": platform.python_version(),
        "detail": sys.executable,
    }


def _check_pip() -> dict:
    rc, out = _run([sys.executable, "-m", "pip", "--version"])
    return {"id": "pip", "status": "ok" if rc == 0 else "missing",
            "version": _first_line(out).split(" from ")[0] if rc == 0 else None,
            "detail": "" if rc == 0 else _first_line(out),
            "fix": "「安裝 deps」會順便處理"} if rc == 0 else {
        "id": "pip", "status": "missing", "version": None,
        "detail": _first_line(out), "fix": "重新建立 venv（web.ps1 -Fresh）"}


def _check_deps() -> dict:
    missing = []
    for mod in ("fastapi", "uvicorn", "serial"):
        try:
            __import__(mod)
        except ImportError:
            missing.append(mod)
    if missing:
        return {"id": "deps", "status": "missing", "version": None,
                "detail": "缺：" + ", ".join(missing),
                "fix": "按「安裝 deps」（pip install -r requirements.txt）"}
    versions = {}
    for mod, key in (("fastapi", "fastapi"), ("uvicorn", "uvicorn")):
        try:
            versions[key] = getattr(__import__(mod), "__version__", "?")
        except ImportError:
            pass
    return {"id": "deps", "status": "ok",
            "version": " ".join(f"{k}{v}" for k, v in versions.items()) or "已安裝",
            "detail": ""}


def _check_esptool() -> dict:
    rc, out = _run([sys.executable, "-m", "esptool", "version"])
    if rc != 0:
        rc2, out2 = _run([sys.executable, "-m", "esptool", "--help"])
        if rc2 != 0:
            return {"id": "esptool", "status": "missing", "version": None,
                    "detail": _first_line(out2 or out),
                    "fix": "按「安裝 esptool」（只影響 server 側 ROM 燒錄）"}
        out = out2
    ver = ""
    for line in out.splitlines():
        if "esptool" in line.lower():
            ver = line.strip()[:80]
            break
    return {"id": "esptool", "status": "ok", "version": ver or "已安裝", "detail": ""}


def _check_cmd(name: str, args: list[str], min_ver: tuple | None = None) -> dict:
    exe = _tool(name)
    if not exe:
        return {"id": name, "status": "missing", "version": None,
                "detail": f"PATH 上搵唔到 {name}",
                "fix": _install_hint(name)}
    rc, out = _run([exe, *args])
    line = _first_line(out)
    status = "ok"
    if rc != 0:
        status = "warn"
    note = ""
    if name == "cmake" and line:
        m = re.search(r"cmake version (\d+)", line)
        if m and int(m.group(1)) >= 4:
            note = ("cmake 4.x：部分 ESP-IDF 版本要加 "
                    "-DCMAKE_POLICY_VERSION_MINIMUM=3.5 才 build 得過")
    return {"id": name, "status": status,
            "version": line or "已安裝", "detail": exe, "note": note}


def _install_hint(name: str) -> str:
    return {
        "git": "Windows: winget install Git.Git ｜ macOS: brew install git",
        "cmake": "Windows: winget install Kitware.CMake ｜ macOS: brew install cmake",
        "ninja": "pip install ninja（或用 IDF 安裝程序一併裝）",
        "node": "Windows: winget install OpenJS.NodeJS ｜ macOS: brew install node",
    }.get(name, f"安裝 {name}")


def _idf_candidates() -> list[Path]:
    cands: list[Path] = []
    env = os.environ.get("IDF_PATH")
    if env:
        cands.append(Path(env))
    home = Path.home()
    cands += [home / "esp" / "esp-idf", home / "esp-idf"]
    if os.name == "nt":
        for base in (Path("C:/Espressif/frameworks"), Path("C:/Espressif")):
            if base.exists():
                try:
                    cands += sorted(base.glob("esp-idf*"))[:5]
                except OSError:
                    pass
    # PATH 上的 idf.py（IDF export 之後）
    idfpy = _tool("idf.py")
    if idfpy:
        cands.append(Path(idfpy).resolve().parent.parent)
    seen, out = set(), []
    for c in cands:
        try:
            rp = c.resolve()
        except OSError:
            continue
        if rp in seen:
            continue
        seen.add(rp)
        if (rp / "tools" / "idf.py").exists() or (rp / "export.ps1").exists() \
                or (rp / "export.sh").exists():
            out.append(rp)
    return out



def idf_env(idf_dir: Path | None = None) -> dict:
    """砌出「可以跑 idf.py」的環境變數，唔需要 export.ps1。

    為什麼要自己砌：`export.ps1` 會用 psutil 偵測父 shell；喺非互動／短命
    process（例如由 server 起的子進程）會 `psutil.NoSuchProcess` 而失敗。
    我們已知所有位置，直接組 PATH 更穩。

    回 {IDF_PATH, IDF_TOOLS_PATH, IDF_PYTHON_ENV_PATH, PATH, PYTHONIOENCODING}。
    """
    d = Path(idf_dir or _idf_candidates()[0] if _idf_candidates() else
             default_idf_dir())
    tools = Path(os.environ.get("IDF_TOOLS_PATH") or (d.parent / "tools"))
    env2 = dict(os.environ)
    env2["IDF_PATH"] = str(d)
    env2["IDF_TOOLS_PATH"] = str(tools)
    env2["PYTHONIOENCODING"] = "utf-8"
    env2["PYTHONUTF8"] = "1"

    # ESP_IDF_VERSION：唔設嘅話，新版 IDF 的 component manager 會
    # `Version.coerce(None)` → TypeError，連 `idf.py set-target` 都跑唔到。
    # 純 git clone 冇 version.txt，所以由 version.txt 或 `git describe` 取。
    ver = None
    vf = d / "version.txt"
    if vf.exists():
        try:
            ver = vf.read_text(encoding="utf-8").strip()
        except OSError:
            ver = None
    if not ver and (d / ".git").exists():
        rc, out = _run(["git", "-C", str(d), "describe", "--tags", "--always"], timeout=8)
        if rc == 0 and out:
            ver = out.splitlines()[0].strip()
    if ver:
        env2["ESP_IDF_VERSION"] = ver.lstrip("v")

    extra: list[str] = []
    pyenv = None
    penv_root = tools / "python_env"
    if penv_root.is_dir():
        for c in sorted(penv_root.glob("idf*_py*_env")):
            pyenv = c
            break
    if pyenv:
        env2["IDF_PYTHON_ENV_PATH"] = str(pyenv)
        extra.append(str(pyenv / "Scripts"))

    # 工具鏈／cmake／ninja 的 bin 目錄
    tdir = tools / "tools"
    if tdir.is_dir():
        for pat in ("**/bin", "*/*/bin"):
            for b in tdir.glob(pat):
                if b.is_dir():
                    name = b.parent.name.lower()
                    if "bin" in str(b).lower() and any(
                            k in str(b).lower() for k in
                            ("gcc", "cmake", "ninja", "riscv", "xtensa",
                             "openocd", "esp-rom", "python")):
                        extra.append(str(b))
    # 暫存目錄：gcc／cmake 會喺 %TEMP% 寫中間檔；受限環境寫唔到會令
    # CMake 的 compiler check 報「<gcc> - broken」（實測）。
    tmp_dir = tools / "tmp"
    try:
        tmp_dir.mkdir(parents=True, exist_ok=True)
        env2["TMP"] = str(tmp_dir)
        env2["TEMP"] = str(tmp_dir)
    except OSError:
        pass

    if extra:
        seen, uniq = set(), []
        for e in extra:
            if e not in seen:
                seen.add(e)
                uniq.append(e)
        env2["PATH"] = ";".join(uniq + [env2.get("PATH", "")])
    return env2


def _check_idf() -> dict:
    cands = _idf_candidates()
    # 自動／預設安裝位置（可能係 <repo>/.idf/esp-idf，唔喺慣例目錄）也要計入
    auto_dir = default_idf_dir()
    if (auto_dir / "tools" / "idf.py").exists() and auto_dir not in cands:
        cands = [auto_dir] + cands
    if not cands:
        return {"id": "idf", "status": "missing", "version": None,
                "detail": "搵唔到 ESP-IDF（IDF_PATH 未設，慣例目錄亦冇）",
                "fix": "按「安裝 ESP-IDF」（會 clone 官方 repo 並跑 install 腳本，約 10–30 分鐘）"}
    root = cands[0]
    version = None
    # 先真跑一次 idf.py --version（比讀檔案可靠；亦驗證 python env／工具鏈齊唔齊）
    try:
        e2 = idf_env(root)
        py = Path(e2.get("IDF_PYTHON_ENV_PATH") or sys.executable) / "Scripts" / "python.exe"
        if not py.exists():
            py = Path(e2.get("IDF_PYTHON_ENV_PATH") or "") / "bin" / "python"
        idfpy = root / "tools" / "idf.py"
        if py.exists() and idfpy.exists():
            rc2, out2 = _run([str(py), str(idfpy), "--version"], timeout=40.0)
            for line in out2.splitlines():
                if re.match(r"^v?\d+\.\d+", line.strip()):
                    version = line.strip()[:40]
                    break
    except Exception:
        version = None
    ver_file = root / "version.txt"
    if ver_file.exists():
        try:
            version = ver_file.read_text(encoding="utf-8").strip()[:40]
        except OSError:
            version = None
    if not version:
        # 由 tools/idf.py 讀 VERSION，避免要 export 環境
        idfpy = root / "tools" / "idf.py"
        if idfpy.exists():
            rc, out = _run([sys.executable, str(idfpy), "--version"])
            version = _first_line(out) if rc == 0 else None
    if not version:
        try:
            import subprocess as sp
            version = sp.run(["git", "-C", str(root), "describe", "--tags", "--always"],
                             capture_output=True, text=True, timeout=10).stdout.strip()[:40]
        except Exception:
            version = None
    return {"id": "idf", "status": "ok", "version": version or "已安裝",
            "detail": str(root)}


def _check_toolchain() -> dict:
    found = [w for w in ("xtensa-esp-elf-gcc", "riscv32-esp-elf-gcc") if _tool(w)]
    if found:
        return {"id": "idf_toolchain", "status": "ok",
                "version": ", ".join(found), "detail": ""}
    # 未 export 環境時，工具鏈可能在 ~/.espressif/tools 或 <idf_dir>/../tools
    bases = [Path.home() / ".espressif" / "tools"]
    env_tools = os.environ.get("IDF_TOOLS_PATH")
    if env_tools:
        bases.insert(0, Path(env_tools))
    bases.insert(1, default_idf_dir().parent / "tools")
    hit_base = None
    for b in bases:
        if b.exists():
            hit_base = b
            break
    base = hit_base if hit_base else bases[0]
    if base and base.exists():
        try:
            hits = list(base.glob("**/bin/*-esp-elf-gcc*"))[:3]
        except OSError:
            hits = []
        if hits:
            return {"id": "idf_toolchain", "status": "warn", "version": "已安裝但未 export",
                    "detail": str(hits[0].parent),
                    "fix": "建置前先跑 export.ps1（Windows）／export.sh（macOS）"}
    return {"id": "idf_toolchain", "status": "missing", "version": None,
            "detail": "搵唔到 xtensa/riscv gcc",
            "fix": "「安裝 ESP-IDF」會一併裝工具鏈"}


def _check_serial() -> dict:
    try:
        import serial  # noqa: F401
        from serial.tools import list_ports
        ports = list(list_ports.comports())
    except ImportError:
        return {"id": "serial_driver", "status": "missing", "version": None,
                "detail": "pyserial 未安裝", "fix": "按「安裝 deps」"}
    except Exception as e:                                  # pragma: no cover
        return {"id": "serial_driver", "status": "warn", "version": None,
                "detail": str(e)[:100]}
    return {"id": "serial_driver", "status": "ok",
            "version": f"{len(ports)} 個序列埠", "detail": ""}



# ── IDF 目標晶片（install 腳本的 --targets／位置參數）──────────────────
# 只列本 repo 會用到／主流的型號。`all` 會裝全部（很大，慎用）。
KNOWN_TARGETS: dict[str, str] = {
    "esp32": "ESP32（經典）",
    "esp32s2": "ESP32-S2",
    "esp32s3": "ESP32-S3",
    "esp32s31": "ESP32-S31",
    "esp32c2": "ESP32-C2",
    "esp32c3": "ESP32-C3",
    "esp32c5": "ESP32-C5",
    "esp32c6": "ESP32-C6",
    "esp32c61": "ESP32-C61",
    "esp32h2": "ESP32-H2",
    "esp32h21": "ESP32-H21",
    "esp32h4": "ESP32-H4",
    "esp32p4": "ESP32-P4",
}
# 注意：以上係「方便 UI 顯示」的清單，**唔係權威**。權威清單由 IDF 本身提供
# （`components/soc/<target>` 或 GitHub API），見 targets_known()。
# 新晶片會陸續出現（例如 esp32s31、esp32c5、esp32c61、esp32h4、esp32h21），
# 所以驗證一定要以 IDF 為準，唔可以靠呢個 dict 擋。
DEFAULT_TARGETS = ["esp32s3", "esp32c3", "esp32c6", "esp32p4"]

TARGET_PRESETS: dict[str, dict] = {
    "scp_mainstream": {
        "label": "S＋C＋P 主流（S3／C3／C6／P4）",
        "targets": ["esp32s3", "esp32c3", "esp32c6", "esp32p4"],
    },
    "s_only": {"label": "只 S 系列（S2／S3）", "targets": ["esp32s2", "esp32s3"]},
    "c_only": {"label": "只 C 系列（C2／C3／C6）", "targets": ["esp32c2", "esp32c3", "esp32c6"]},
    "p_only": {"label": "只 P 系列（P4）", "targets": ["esp32p4"]},
    "all": {"label": "全部（很大）", "targets": ["all"]},
}



# ── 權威 target 清單（IDF 為準）──────────────────────────────────────
_TARGETS_CACHE: dict = {"at": 0.0, "targets": [], "source": None}
_TARGETS_TTL = 3600.0


def idf_targets_from_dir(idf_dir: Path | str) -> list[str]:
    """由已安裝的 IDF 讀 `components/soc/*`（最快、最準；唔起 subprocess）。"""
    soc = Path(idf_dir) / "components" / "soc"
    if not soc.is_dir():
        return []
    try:
        return sorted(p.name for p in soc.iterdir()
                      if p.is_dir() and p.name.startswith("esp32"))
    except OSError:
        return []


def idf_targets_from_github(timeout: float = 20.0) -> list[str]:
    """由 GitHub API 讀 `components/soc` 目錄名（離線時回空）。"""
    import json as _json
    import urllib.request

    url = ("https://api.github.com/repos/espressif/esp-idf/contents/"
           "components/soc?ref=master")
    try:
        req = urllib.request.Request(url, headers={
            "User-Agent": "esp-webmaster", "Accept": "application/vnd.github+json"})
        with urllib.request.urlopen(req, timeout=timeout) as r:
            data = _json.loads(r.read().decode("utf-8", errors="replace"))
        return sorted(d["name"] for d in data
                      if isinstance(d, dict) and d.get("type") == "dir"
                      and str(d.get("name", "")).startswith("esp32"))
    except Exception:
        return []


def targets_known(force: bool = False) -> tuple[list[str], str]:
    """回 (可用晶片清單, 來源)。順序：已安裝的 IDF → GitHub → 內建清單。"""
    now = time.time()
    if not force and _TARGETS_CACHE["targets"] and now - _TARGETS_CACHE["at"] < _TARGETS_TTL:
        return _TARGETS_CACHE["targets"], _TARGETS_CACHE["source"] or "cache"

    cands = _idf_candidates()
    if cands:
        got = idf_targets_from_dir(cands[0])
        if got:
            _TARGETS_CACHE.update({"at": now, "targets": got, "source": f"IDF {cands[0]}"})
            return got, f"IDF {cands[0]}"
    got = idf_targets_from_github()
    if got:
        _TARGETS_CACHE.update({"at": now, "targets": got, "source": "GitHub esp-idf"})
        return got, "GitHub esp-idf"
    builtin = sorted(KNOWN_TARGETS)
    _TARGETS_CACHE.update({"at": now, "targets": builtin, "source": "內建清單（可能過時）"})
    return builtin, "內建清單（可能過時）"


def _split_targets(value) -> list[str]:
    """各種輸入 → 小寫晶片名清單（只做拆分，唔驗證）。"""
    if value is None or value == "":
        return []
    if isinstance(value, str):
        raw = value.strip()
        if raw in TARGET_PRESETS:                       # 允許直接傳 preset 名
            raw = ",".join(TARGET_PRESETS[raw]["targets"])
        # 容錯：有人會寫 "esp32s3 esp32c3" 或者 ["esp32s3"] 的 JSON 字串
        raw = raw.replace("[", "").replace("]", "").replace('"', "")
        parts = re.split(r"[,;\s]+", raw)
        return [x.strip().lower() for x in parts if x.strip()]
    if isinstance(value, (list, tuple, set)):
        out = []
        for x in value:
            out.extend(_split_targets(x))
        return out
    return [str(value).strip().lower()]


def normalize_targets(value, *, strict: bool = True) -> str:
    """把 "esp32s3,esp32c3"／清單／preset 名 → 合法的逗號字串。

    strict=True （預設，API 用）：有唔認識的晶片 → raise ValueError（帶可用名單）。
    strict=False（設定檔／開機自動安裝用）：**丟掉壞值**、保留好的；
      一個都冇剩就落返 DEFAULT_TARGETS。回傳值永遠合法，唔會因為手誤而整項失敗。
    `all` 只可以單獨用。
    """
    items = _split_targets(value)
    if not items:
        return ",".join(DEFAULT_TARGETS)
    if "all" in items:
        if len(items) > 1:
            if strict:
                raise ValueError("`all` 唔可以同其他晶片一齊用")
            return "all"
        return "all"
    known, source = targets_known()
    known_set = set(known) | set(KNOWN_TARGETS)
    bad = [x for x in items if x not in known_set]
    good: list[str] = []
    seen = set()
    for x in items:                                     # 去重、保留次序
        if x not in seen:
            seen.add(x)
            good.append(x)
    if bad and strict:
        raise ValueError(
            f"唔認識嘅晶片：{', '.join(bad)}；可用：{', '.join(known)} 或 all"
            f"（來源：{source}）")
    if not good:
        return ",".join(DEFAULT_TARGETS)
    return ",".join(good)


def targets_or_default(value) -> tuple[str, list[str]]:
    """寬鬆版：回 (targets, 唔認識的項目)。

    唔認識**唔會丟**——可能係新晶片（例如 esp32s31）；只會回報，由 IDF 自己判斷。
    """
    items = _split_targets(value)
    known, _src = targets_known()
    known_set = set(known) | set(KNOWN_TARGETS)
    unknown = [x for x in items if x not in known_set and x != "all"]
    return normalize_targets(value, strict=False), unknown


def targets_label(targets: str) -> str:
    if targets == "all":
        return "all（全部晶片）"
    return "／".join(KNOWN_TARGETS.get(x, x) for x in targets.split(","))


# ── 開機自動補齊（唔阻塞、唔無限重試）────────────────────────────────
AUTO_MARKER = Path(os.environ.get("ESPWM_AUTO_MARKER") or (BASE / ".env-auto.json"))
AUTO_COOLDOWN = 6 * 3600.0          # 同一個項目失敗之後 6 小時內唔再自動試


def _auto_state_read() -> dict:
    try:
        import json as _json
        return _json.loads(AUTO_MARKER.read_text(encoding="utf-8"))
    except Exception:
        return {}


def _auto_state_write(state: dict) -> None:
    try:
        import json as _json
        AUTO_MARKER.parent.mkdir(parents=True, exist_ok=True)
        AUTO_MARKER.write_text(_json.dumps(state, ensure_ascii=False, indent=2),
                               encoding="utf-8")
    except OSError:
        pass


def auto_plan(mode: str, rep: dict | None = None) -> list[dict]:
    """決定「開機應該自動裝咩」。mode: off / safe / all。

    - `safe`：只補**便宜且必需**的（deps、esptool）；唔會自動裝 IDF。
    - `all` ：連 IDF 都自動裝（用 settings 的版本／晶片／路徑）。
    - 6 小時內失敗過的項目會跳過（避免每次開機都重試、拖慢啟動）。
    """
    if mode == "off":
        return []
    rep = rep or report()
    ready = rep.get("ready") or {}
    ids = {t["id"]: t for t in rep.get("tools", [])}
    state = _auto_state_read()
    now = time.time()
    plan: list[dict] = []

    def cooling(what: str) -> bool:
        last = (state.get(what) or {})
        if not last.get("at"):
            return False
        if last.get("ok"):
            return False
        return (now - float(last["at"])) < AUTO_COOLDOWN

    if not ready.get("run_server") and not cooling("deps"):
        plan.append({"what": "deps", "reason": "Web deps 缺（server 開唔到）"})
    et = ids.get("esptool", {})
    if not ready.get("flash_esptool") and et.get("status") != "ok" \
            and not cooling("esptool"):
        plan.append({"what": "esptool", "reason": "server 側 ROM 燒錄唔可用（缺 esptool）"})
    elif et.get("status") == "ok" and et.get("latest") and et.get("outdated") \
            and not cooling("esptool"):
        # 「有新版本」也算需要處理（否則用戶以為程式冇幫佢更新）
        plan.append({"what": "esptool",
                     "reason": f"esptool 可更新 {et.get('installed')} → {et.get('latest')}"})
    if mode == "all" and not ready.get("build_firmware") and not cooling("idf"):
        d = default_idf_dir()
        if (d / ".git").exists() or _writable_dir(d.parent):
            plan.append({"what": "idf", "reason": "建置 firmware 唔可用（缺 IDF／工具鏈）"})
        else:
            # 冇一個候選位置可寫：唔好白白跑 1–2 GB（實測會喺 git clone 就失敗）
            cands = idf_dir_candidates()
            plan.append({
                "what": "idf", "blocked": True,
                "reason": "建置 firmware 唔可用，但冇可寫的安裝位置",
                "hint": ("所有預設位置都唔可寫："
                         + "、".join(str(c) for c in cands[:3])
                         + "。請喺 settings.json 設 "
                           '"env": {"idf_dir": "D:/esp/esp-idf"}（或任何你寫得入的位置），'
                           "或者手動安裝 IDF 後設 IDF_PATH。"),
            })
    return plan


def auto_mark(what: str, ok: bool) -> None:
    st = _auto_state_read()
    st[what] = {"at": time.time(), "ok": bool(ok)}
    _auto_state_write(st)


def auto_last() -> dict:
    return _auto_state_read()


def report() -> dict:
    """完整環境報告（只讀，唔會裝任何東西）。"""
    live_targets, targets_source = targets_known()
    # 套件新鮮度：問 PyPI（快取 6 小時）。失敗唔增加延遲（有 timeout）。
    global _updates_cache
    _updates_cache = updates()
    up_by_pkg = {u["pkg"]: u for u in _updates_cache}
    checks = [
        _check_python(), _check_pip(), _check_deps(), _check_esptool(),
        _check_cmd("git", ["--version"]),
        _check_cmd("cmake", ["--version"]),
        _check_cmd("ninja", ["--version"]),
        _check_cmd("node", ["--version"]),
        _check_idf(), _check_toolchain(), _check_serial(),
    ]
    for c in checks:
        info = TOOL_INFO.get(c["id"])
        if info:
            c["label"], c["impact"] = info
        c.setdefault("fix", "")
    by_id = {c["id"]: c for c in checks}

    # 把版本新鮮度貼到對應工具上（esptool／deps）
    et = by_id.get("esptool")
    if et and "esptool" in up_by_pkg:
        u = up_by_pkg["esptool"]
        et["installed"] = u["installed"]
        et["latest"] = u["latest"]
        et["outdated"] = u["outdated"]
        et["updateStatus"] = u["status"]
        if u["outdated"]:
            et["version"] = f"{u['installed']} → {u['latest']}（可更新）"
    deps = by_id.get("deps")
    if deps:
        dep_up = [u for u in _updates_cache
                  if u["pkg"] in ("fastapi", "uvicorn", "pyserial") and u["outdated"]]
        if dep_up:
            deps["updateStatus"] = "可更新：" + "、".join(
                f"{u['pkg']} {u['installed']}→{u['latest']}" for u in dep_up)


    def ok(i: str) -> bool:
        return by_id.get(i, {}).get("status") == "ok"

    # 四種「能力」：分清楚「燒得到」同「建置得到」
    ready = {
        "run_server": ok("deps"),
        "flash_nc4_ota": ok("deps") and ok("serial_driver"),
        "flash_esptool": ok("esptool") and ok("serial_driver"),
        "flash_browser": True,          # 只取決於瀏覽器，server 側唔知
        # 工具鏈「已安裝但未 export」（warn）都算可用——建置前 export 就得。
        # 只有 missing 才算唔可建置。
        "build_firmware": ok("idf") and ok("cmake") and ok("ninja") and (
            ok("idf_toolchain")
            or by_id.get("idf_toolchain", {}).get("status") == "warn"),
    }
    return {
        "os": {"platform": platform.platform(), "machine": platform.machine(),
               "pythonExe": sys.executable, "osName": os.name},
        "tools": checks,
        "ready": ready,
        "idfCandidates": [str(p) for p in _idf_candidates()],
        "installDir": str(default_idf_dir()),
        "installDirCandidates": writable_candidates(),
        "missing": [c["id"] for c in checks if c["status"] == "missing"],
        "warn": [c["id"] for c in checks if c["status"] == "warn"],
        "targets": {
            # live = 權威清單（由已安裝 IDF 的 components/soc 或 GitHub 取）
            "live": live_targets,
            "source": targets_source,
            "known": KNOWN_TARGETS,        # 只作中文標籤用
            "default": DEFAULT_TARGETS,
            "presets": {k: v["label"] for k, v in TARGET_PRESETS.items()},
        },
        "updates": _updates_cache,
        "auto": {"last": auto_last(), "cooldownSec": AUTO_COOLDOWN},
    }


# ── 安裝（唯一會寫入的地方）────────────────────────────────────────────


# ── ESP-IDF 版本（GitHub tags）──────────────────────────────────────
# 為什麼要問 GitHub：版本名唔可以靠猜。實測 **6.1.0 唔存在**——
# ESP-IDF 的分支/修補慣例係 6.1（發佈）＋6.1.1（修補），冇 .0。
_VERSION_CACHE: dict = {"at": 0.0, "tags": [], "error": None}
_VERSION_TTL = 3600.0
_PRERELEASE = ("-dev", "-rc", "-beta", "-alpha")


def fetch_idf_versions(force: bool = False, timeout: float = 20.0) -> dict:
    """由 GitHub 取 ESP-IDF tags（快取 1 小時）。失敗回舊快取＋error，唔 raise。"""
    import json as _json
    import urllib.request

    now = time.time()
    if not force and _VERSION_CACHE["tags"] and now - _VERSION_CACHE["at"] < _VERSION_TTL:
        return dict(_VERSION_CACHE)
    url = ("https://api.github.com/repos/espressif/esp-idf/tags?per_page=100")
    try:
        req = urllib.request.Request(url, headers={
            "User-Agent": "esp-webmaster", "Accept": "application/vnd.github+json"})
        with urllib.request.urlopen(req, timeout=timeout) as r:
            data = _json.loads(r.read().decode("utf-8", errors="replace"))
        tags = [d["name"] for d in data if isinstance(d, dict) and d.get("name")]
        if tags:
            _VERSION_CACHE.update({"at": now, "tags": tags, "error": None})
    except Exception as e:                       # 離線／被限流都唔應該阻住安裝
        _VERSION_CACHE["error"] = f"{type(e).__name__}: {e}"[:160]
    return dict(_VERSION_CACHE)


def stable_versions(tags: list[str]) -> list[str]:
    """只留正式版（去掉 -dev／-rc／-beta／-alpha），保持 GitHub 的次序。"""
    return [t for t in tags if not any(s in t.lower() for s in _PRERELEASE)]


def normalize_version(want: str | None, tags: list[str] | None = None) -> tuple[str | None, str]:
    """把用戶輸入變成真實存在的 tag。回 (tag|None, 說明)。

    規則（順序）：
      1. 空白／latest → 最新正式版
      2. 完全吻合（6.1.1）
      3. 補  前綴（6.1.1 → 6.1.1）
      4. 去掉尾部 .0（6.1.0 → 6.1）——**ESP-IDF 冇 .0 tag**
      5. 同系列最新修補（6.1 已存在就用它；否則 6.1.x 最大者）
    """
    tags = tags if tags is not None else stable_versions(fetch_idf_versions()["tags"])
    if not tags:
        return (want if want and want not in ("latest",) else None), \
               "攞唔到版本清單（離線？）；會照用你輸入的值，失敗時請檢查版本名"
    raw = (want or "").strip()
    if raw in ("", "latest", "stable"):
        return tags[0], f"用最新正式版 {tags[0]}"
    cand = raw if raw.startswith("v") else "v" + raw
    if cand in tags:
        return cand, "" if cand == raw else f"{raw} → {cand}"
    if cand.endswith(".0") and cand[:-2] in tags:
        return cand[:-2], f"{raw} 唔存在（ESP-IDF 冇 .0 tag）→ 用 {cand[:-2]}"
    # 同系列：v6.1 → v6.1.x 最大
    if re.fullmatch(r"v\d+\.\d+(?:\.\d+)?", cand):
        series = re.sub(r"\.\d+$", "", cand) if cand.count(".") >= 2 else cand
        same = [x for x in tags if x == series or x.startswith(series + ".")]
        if same:
            def key(v: str) -> tuple:
                return tuple(int(n) for n in re.findall(r"\d+", v))
            best = max(same, key=key)
            note = f"{raw} 唔存在" + (f" → 用同系列最新 {best}" if best != series else f" → 用 {series}")
            return best, note
    return cand, f"警告：{raw} 唔在官方 tag 清單中（照試；失敗請改版本）"



# ── pip 套件版本新鮮度（PyPI）────────────────────────────────────────
# 為什麼要問 PyPI：使用者見到「esptool OK v5.6.0」會以為「冇幫我更新」。
# 要有「已是最新」／「可更新 x → y」才講得清。
_PYPI_CACHE: dict = {}
_updates_cache: list = []
_PYPI_TTL = 6 * 3600.0
_WATCH_PKGS = ("esptool", "fastapi", "uvicorn", "pyserial")


def pypi_latest(pkg: str, timeout: float = 10.0) -> str | None:
    """問 PyPI 最新版本；失敗回 None（離線唔應該令檢查失敗）。"""
    now = time.time()
    hit = _PYPI_CACHE.get(pkg)
    if hit and now - hit["at"] < _PYPI_TTL:
        return hit["version"]
    import json as _json
    import urllib.request

    try:
        req = urllib.request.Request(f"https://pypi.org/pypi/{pkg}/json",
                                     headers={"User-Agent": "esp-webmaster"})
        with urllib.request.urlopen(req, timeout=timeout) as r:
            data = _json.loads(r.read().decode("utf-8", errors="replace"))
        ver = str(data.get("info", {}).get("version") or "") or None
    except Exception:
        ver = None
    _PYPI_CACHE[pkg] = {"at": now, "version": ver}
    return ver


def installed_version(pkg: str) -> str | None:
    """由 importlib.metadata 取已安裝版本（唔靠 import 該套件）。"""
    try:
        from importlib.metadata import version as _v
        return _v(pkg)
    except Exception:
        return None


def _parse_ver(v: str | None) -> tuple:
    import re as _re
    if not v:
        return ()
    return tuple(int(x) for x in _re.findall(r"\d+", v)[:4])


def updates(check_remote: bool = True) -> list[dict]:
    """回傳有更新的套件清單：[{pkg, installed, latest, outdated}]。"""
    out: list[dict] = []
    for pkg in _WATCH_PKGS:
        cur = installed_version(pkg)
        latest = pypi_latest(pkg) if check_remote else None
        outdated = bool(cur and latest and _parse_ver(latest) > _parse_ver(cur))
        out.append({"pkg": pkg, "installed": cur, "latest": latest,
                    "outdated": outdated,
                    "status": ("可更新" if outdated
                               else ("已是最新" if latest else "未知（離線？）"))})
    return out


def _writable_dir(path: Path) -> bool:
    """試試可唔可以喺呢個路徑建立（唔會留低垃圾）。"""
    try:
        path.mkdir(parents=True, exist_ok=True)
        probe = path / ".espwm-write-test"
        probe.write_text("x", encoding="utf-8")
        probe.unlink(missing_ok=True)
        return True
    except (OSError, PermissionError):
        return False


def idf_dir_candidates() -> list[Path]:
    """IDF 安裝位置候選（順序＝優先）。

    為什麼要候選：某些環境（公司機／沙箱／受控資料夾）唔准喺 `~/esp` 建檔，
    實測 WinError 5（存取被拒）就係喺家目錄的 esp 目錄。硬用一個位置就會
    「開機自動安裝永遠失敗」，要自動退到可寫的位置。
    """
    out: list[Path] = []
    envdir = os.environ.get("ESPWM_IDF_DIR") or os.environ.get("IDF_INSTALL_DIR")
    if envdir:
        out.append(Path(envdir).expanduser())
    home = Path.home()
    out.append(home / "esp" / "esp-idf")
    if os.name == "nt":
        local = os.environ.get("LOCALAPPDATA")
        if local:
            out.append(Path(local) / "esp" / "esp-idf")
    else:
        out.append(home / ".local" / "share" / "esp" / "esp-idf")
    out.append(home / ".espressif" / "esp-idf")
    # repo 內兜底：某些環境（沙箱／受控機）唔准寫家目錄，但 workspace 一定寫得入。
    # 缺點係 1–2 GB 會放喺 repo（已 gitignore）；用戶自己雙擊啟動時會先用 ~/esp。
    out.append(TOOL_ROOT.parent / ".idf" / "esp-idf")
    tmp = os.environ.get("TEMP") or os.environ.get("TMP")
    if tmp:
        out.append(Path(tmp) / "esp" / "esp-idf")
    seen, uniq = set(), []
    for c in out:
        if c not in seen:
            seen.add(c)
            uniq.append(c)
    return uniq


def writable_candidates() -> list[dict]:
    """[{path, writable}] —— 給 UI「用可寫位置」用。"""
    return [{"path": str(c), "writable": _writable_dir(c.parent)}
            for c in idf_dir_candidates()]


def first_writable_idf_dir() -> str | None:
    for item in writable_candidates():
        if item["writable"]:
            return item["path"]
    return None


def default_idf_dir() -> Path:
    """第一個**可寫**的候選；全部唔可寫就回第一個（並由 install 報清楚原因）。"""
    cands = idf_dir_candidates()
    for c in cands:
        parent = c.parent
        if c.exists() or _writable_dir(parent):
            return c
    return cands[0]


def install_command(what: str, target: str = "all", idf_dir: Path | None = None) -> list[str]:
    """組出「官方」安裝命令（唔執行）。

    what: deps / esptool / idf
    target: IDF 目標晶片（esp32s3 / esp32p4 / all）
    """
    if what == "deps":
        req = BASE / "requirements.txt"
        return [sys.executable, "-m", "pip", "install", "--no-cache-dir",
                "--disable-pip-version-check", "-r", str(req)]
    if what == "esptool":
        # --upgrade：唔止「裝」，而係「升到最新」（PyPI 上的版本）
        return [sys.executable, "-m", "pip", "install", "--upgrade",
                "--no-cache-dir", "--disable-pip-version-check", "esptool"]
    if what == "idf":
        d = Path(idf_dir or default_idf_dir())
        target = normalize_targets(target)
        if _is_windows():
            # install.ps1 會自己裝工具鏈（python env + gcc + cmake + ninja）
            return ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
                    "-File", str(d / "install.ps1"), target]
        return ["bash", str(d / "install.sh"), target]
    raise ValueError(f"唔認識嘅安裝項目：{what}")


def clone_command(idf_dir: Path | None = None, version: str | None = None) -> list[str]:
    """clone 官方 repo；version 係真實存在的 tag（例如 v6.1.1，見 normalize_version）。"""
    d = Path(idf_dir or default_idf_dir())
    cmd = ["git", "clone", "--recursive", "--depth", "1"]
    if version:
        cmd += ["--branch", version]
    cmd += ["https://github.com/espressif/esp-idf.git", str(d)]
    return cmd


class Installer:
    """一次只跑一個安裝工作；輸出保留在 ring 裡給 UI／AI 讀。"""

    MAX_LINES = 4000

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._lines: list[str] = []
        self._thread: threading.Thread | None = None
        self.running = False
        self.what: str | None = None
        self.rc: int | None = None
        self.started: float | None = None
        self.finished: float | None = None
        self.steps: list[str] = []
        # 環境類錯誤（權限／缺工具／資料夾唔可寫）唔應該觸發 6 小時冷卻：
        # 用戶改完設定就要即刻可以再試。
        self.env_error: bool = False

    def log(self, text: str) -> None:
        with self._lock:
            for line in str(text).rstrip().splitlines() or [""]:
                self._lines.append(line)
            if len(self._lines) > self.MAX_LINES:
                del self._lines[: len(self._lines) - self.MAX_LINES]

    def snapshot(self, since: int = 0) -> dict:
        with self._lock:
            lines = self._lines[since:]
            total = len(self._lines)
        return {
            "running": self.running, "what": self.what, "rc": self.rc,
            "total": total, "since": since, "lines": lines,
            "started": self.started, "finished": self.finished,
            "elapsed": round((self.finished or time.time()) - (self.started or time.time()), 1)
            if self.started else None,
            "steps": list(self.steps),
        }

    def start(self, what: str, target: str = "all", idf_dir: Path | None = None,
              confirm: bool = False, version: str | None = None) -> dict:
        if not confirm:
            raise ValueError("要明確確認（confirm: true）才會執行安裝")
        if what not in ("deps", "esptool", "idf"):
            raise ValueError(f"唔認識嘅安裝項目：{what}（可用：deps／esptool／idf）")
        if what == "idf":
            target = normalize_targets(target)          # 驗證＋預設＋preset 展開
        with self._lock:
            if self.running:
                raise ValueError("已經有一個安裝工作喺跑，等佢完成或者等 timeout")
            self._lines = []
            self.steps = []
            self.running = True
            self.what = what
            self.rc = None
            self.started = time.time()
            self.finished = None
        self._thread = threading.Thread(
            target=self._run, args=(what, target, idf_dir, version), daemon=True)
        self._thread.start()
        return self.snapshot()

    # 內部：實際跑
    def _popen(self, cmd: list[str], cwd: Path | None = None,
               extra_env: dict | None = None) -> int:
        self.log("$ " + " ".join(cmd))
        env2 = None
        if extra_env:
            env2 = dict(os.environ)
            env2.update({k: str(v) for k, v in extra_env.items()})
            for k, v in extra_env.items():
                self.log(f"   （環境變數）{k}={v}")
        try:
            p = subprocess.Popen(cmd, cwd=str(cwd) if cwd else None, env=env2,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 text=True, errors="replace", bufsize=1)
        except (OSError, subprocess.SubprocessError) as e:
            self.log(f"[啟動失敗] {e}")
            return -1
        assert p.stdout is not None
        for line in p.stdout:
            self.log(line.rstrip())
        return p.wait()

    def _run(self, what: str, target: str, idf_dir: Path | None,
             version: str | None = None) -> None:
        try:
            if what in ("deps", "esptool"):
                self.steps = [f"pip install（{what}）"]
                rc = self._popen(install_command(what))
            elif what == "idf":
                d = Path(idf_dir or default_idf_dir())
                tag, note = normalize_version(version)
                if note:
                    self.log(f"[版本] {note}")
                if tag:
                    self.log(f"[版本] 用 {tag}")
                self.steps = [f"clone ESP-IDF {tag or ""} → {d}", f"install（{target}）"]
                if (d / ".git").exists():
                    self.log(f"{d} 已經係 IDF repo，跳過 clone")
                    if tag:
                        self.log("（提示：已有 repo 唔會自動切版本；要換版本請先刪掉該資料夾）")
                    rc = 0
                else:
                    if d.exists() and any(d.iterdir()):
                        self.log(f"[錯誤] {d} 唔係空資料夾又唔似 IDF repo，為安全起見停手")
                        self.rc = -2
                        return
                    try:
                        d.parent.mkdir(parents=True, exist_ok=True)
                    except OSError as e:
                        self.log(f"[錯誤] 開唔到安裝資料夾：{d.parent}")
                        self.log(f"       原因：{e}")
                        self.log("       解決：喺 settings.json 設 env.idf_dir 去一個"
                                 "你寫得入的位置（例如 D:\\esp\\esp-idf），或用 UI"
                                 "「環境」頁的「IDF 安裝路徑」欄。")
                        self.log("       可寫候選：")
                        for c in idf_dir_candidates():
                            mark = "OK " if _writable_dir(c.parent) else "x  "
                            self.log(f"         {mark}{c}")
                        self.rc = -3
                        return
                    rc = self._popen(clone_command(d, tag))
                if rc == 0:
                    tools_path = d.parent / "tools"
                    tmp_dir = tools_path / "tmp"
                    pip_cache = tools_path / "pip-cache"
                    for sub in (tmp_dir, pip_cache):
                        try:
                            sub.mkdir(parents=True, exist_ok=True)
                        except OSError:
                            pass
                    # 為什麼要全部指走：家目錄（%LOCALAPPDATA%\pip\cache、%TEMP%）
                    # 在受限環境寫唔入，pip 建 wheel 會 [WinError 5] 而整個安裝失敗。
                    # 實測：工具鏈下載成功，但 pip 建 esptool wheel 時撞到呢個問題。
                    rc = self._popen(install_command(what, target, d), cwd=d,
                                     extra_env={
                                         "IDF_TOOLS_PATH": tools_path,
                                         "PIP_CACHE_DIR": pip_cache,
                                         "TMP": tmp_dir,
                                         "TEMP": tmp_dir,
                                     })
                    self.log("")
                    self.log("完成之後，建置前要 export 環境（記住 IDF_TOOLS_PATH）：")
                    self.log(f"  PowerShell : $env:IDF_TOOLS_PATH='{d.parent / 'tools'}'; "
                             f"& '{d}\\export.ps1'")
                    self.log(f"  bash       : export IDF_TOOLS_PATH='{d.parent / 'tools'}'; "
                             f". '{d}/export.sh'")
                    self.log("或者直接在 repo 根跑 make.py（它會自己找 IDF_PATH）")
            else:
                self.log(f"[錯誤] 唔認識嘅項目：{what}")
                rc = -2
            self.rc = rc
            blob = "\n".join(self.snapshot(0)["lines"]).lower()
            self.env_error = rc < 0 or any(k in blob for k in (
                "permission denied", "存取被拒", "permissionerror",
                "could not create work tree", "not found", "command not found"))
            self.log(f"[結束] exit code = {rc}"
                     + ("（環境問題；重開會即刻再試）" if self.env_error else ""))
        except Exception as e:                                # pragma: no cover
            self.log(f"[例外] {e}")
            self.rc = -1
        finally:
            self.running = False
            self.finished = time.time()


INSTALLER = Installer()
