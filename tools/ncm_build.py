#!/usr/bin/env python3
"""獨立任務模組編譯工具（ncm build）

把 `tools/ncm_samples/` 的任務模組（WAT／WASM；C 預留）編譯、打包成 `.nct`，
唔需要成個 firmware 一起 build。工具本身只依賴 Python 標準庫＋外部工具
（wat2wasm 或 clang）。

用法:
    python3 tools/ncm_build.py                  # 編譯全部樣本
    python3 tools/ncm_build.py hello_task       # 只編一個
    python3 tools/ncm_build.py --list           # 列出可編譯的任務
    python3 tools/ncm_build.py --out build/ncm  # 自訂輸出目錄
    python3 tools/ncm_build.py --clean          # 清空輸出目錄
    python3 tools/ncm_build.py hello_task --install /Volumes/NC-xxxx/tasks
    python3 tools/ncm_build.py --device         # 自動找 /Volumes/NC-*/tasks 安裝

每個任務 = 三個檔（同一個名字）：
    <name>.wat          （或 .c／.wasm）payload 源碼
    <name>.build.json   打包參數（可選；缺省用預設）
    <name>.meta.json    名稱／描述／usage（可選）

`.build.json` 欄位（全部可選）：
    {"kind": "task", "fps": 1, "core": -1, "layer": 4, "prio": 3,
     "mem": 65536, "stack": 4096, "budget_us": 20000, "px": 0,
     "kind_flag": "normal", "extra": ["--cmd", "..."]}

C 模組（未來）：
    clang --target=wasm32 -nostdlib ... 需要 wasm-ld（brew install lld）。
    現在如果 .c 存在但工具鏈唔齊，會清楚報錯，不會靜靜跳過。
"""

import argparse
import glob
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SAMPLES = ROOT / "tools" / "ncm_samples"
NCM_PY = ROOT / "tools" / "ncm.py"
DEFAULT_OUT = SAMPLES / "out"

DEFAULTS = {
    "kind": "task",
    "fps": 1,
    "core": -1,
    "layer": 4,
    "prio": 3,
    "mem": 65536,
    "stack": 4096,
    "budget_us": 20000,
    "px": 0,
    "kind_flag": "normal",
    "extra": [],
}


def die(msg: str) -> "NoReturn":
    print(f"ncm_build: {msg}", file=sys.stderr)
    sys.exit(1)


def find_compiler(suffix: str):
    """回傳可用的編譯器路徑（wat2wasm／clang）；冇就 None。"""
    if suffix == ".wat":
        exe = shutil.which("wat2wasm")
        return exe
    if suffix == ".c":
        if shutil.which("clang") and shutil.which("wasm-ld"):
            return shutil.which("clang")
        return None
    return None


def task_names():
    names = set()
    for pat in ("*.wat", "*.c", "*.wasm"):
        for p in SAMPLES.glob(pat):
            names.add(p.stem)
    return sorted(names)


def load_spec(name: str):
    spec = dict(DEFAULTS)
    path = SAMPLES / f"{name}.build.json"
    if path.exists():
        with open(path, "r", encoding="utf-8") as f:
            user = json.load(f)
        if not isinstance(user, dict):
            die(f"{path.name} 必須是 JSON object")
        unknown = set(user) - set(DEFAULTS)
        if unknown:
            die(f"{path.name} 有未知欄位：{sorted(unknown)}")
        spec.update(user)
    return spec


def build_one(name: str, out_dir: Path, install_dir: Path | None, verbose: bool) -> Path:
    spec = load_spec(name)
    src = None
    for ext in (".wat", ".c", ".wasm"):
        cand = SAMPLES / f"{name}{ext}"
        if cand.exists():
            src = cand
            break
    if src is None:
        die(f"找不到 {name}.wat／.c／.wasm")

    out_dir.mkdir(parents=True, exist_ok=True)

    if src.suffix == ".wasm":
        wasm = src
    elif src.suffix == ".wat":
        cc = find_compiler(".wat")
        if cc is None:
            die("編譯 .wat 需要 wat2wasm（macOS: brew install wabt）")
        wasm = out_dir / f"{name}.wasm"
        subprocess.run([cc, str(src), "-o", str(wasm)], check=True)
    elif src.suffix == ".c":
        cc = find_compiler(".c")
        if cc is None:
            die("編譯 .c 需要 clang（wasm32 target）＋ wasm-ld（macOS: brew install lld）")
        wasm = out_dir / f"{name}.wasm"
        cmd = [
            cc, "--target=wasm32", "-O2", "-nostdlib", "-Wl,--no-entry",
            "-Wl,--export=task_tick", "-Wl,--export=task_init",
            "-Wl,--export=memory", "-o", str(wasm), str(src),
        ]
        subprocess.run(cmd, check=True)

    nct = out_dir / f"{name}.nct"
    cmd = [
        sys.executable, str(NCM_PY), "pack",
        "--payload", str(wasm), "--out", str(nct),
        "--kind", spec["kind"],
        "--fps", str(spec["fps"]),
        "--core", str(spec["core"]),
        "--layer", str(spec["layer"]),
        "--prio", str(spec["prio"]),
        "--mem", str(spec["mem"]),
        "--stack", str(spec["stack"]),
        "--budget-us", str(spec["budget_us"]),
        "--px", str(spec["px"]),
    ]
    if spec["kind_flag"] and spec["kind"] != "task":
        cmd += ["--kind", spec["kind_flag"]]
    meta = SAMPLES / f"{name}.meta.json"
    if meta.exists():
        cmd += ["--meta", str(meta)]
    for extra in spec["extra"]:
        cmd.append(str(extra))
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)

    if verbose:
        out = subprocess.run([sys.executable, str(NCM_PY), "dump", str(nct)],
                             check=True, capture_output=True, text=True).stdout
        print(out.rstrip())

    size = nct.stat().st_size
    print(f"ncm_build: {name}.nct  {size} B  →  {nct.relative_to(ROOT)}")

    if install_dir is not None:
        install_dir.mkdir(parents=True, exist_ok=True)
        dst = install_dir / nct.name
        shutil.copy2(nct, dst)
        # macOS 的 AppleDouble 檔（._xxx）也會以 .nct 結尾，loader 會拒收 → 清掉
        host = install_dir / f"._{nct.name}"
        if host.exists():
            host.unlink()
        print(f"ncm_build: 已安裝 → {dst}")
    return nct


def detect_install_dir():
    volumes = sorted(glob.glob("/Volumes/NC-*"))
    if not volumes:
        die("找不到 /Volumes/NC-*（MSC 未掛載？）")
    if len(volumes) > 1:
        die(f"找到多個 NC 磁碟：{volumes}；請用 --install DIR 指定")
    return Path(volumes[0]) / "tasks"


def main():
    ap = argparse.ArgumentParser(description="獨立編譯 ncm 任務模組（.nct）")
    ap.add_argument("tasks", nargs="*", help="任務名（省略 = 全部）")
    ap.add_argument("--list", action="store_true", help="列出可編譯的任務")
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT, help="輸出目錄")
    ap.add_argument("--clean", action="store_true", help="清空輸出目錄")
    ap.add_argument("--install", type=Path, help="編完複製 .nct 到指定目錄（例如 MSC 的 tasks/）")
    ap.add_argument("--device", action="store_true", help="自動安裝到 /Volumes/NC-*/tasks")
    ap.add_argument("-v", "--verbose", action="store_true", help="印出容器 dump")
    args = ap.parse_args()

    if args.clean:
        if args.out.exists():
            for p in args.out.glob("*.wasm"):
                p.unlink()
            for p in args.out.glob("*.nct"):
                p.unlink()
        print(f"ncm_build: 已清空 {args.out.relative_to(ROOT)}")

    names = args.tasks or task_names()
    if args.list:
        for n in names:
            print(n)
        return

    if not names:
        die(f"{SAMPLES.relative_to(ROOT)} 冇任何 .wat／.c／.wasm")

    unknown = [n for n in names if n not in task_names()]
    if unknown:
        die(f"未知任務：{unknown}（可用：{task_names()}）")

    install = args.install
    if args.device:
        install = detect_install_dir()

    for n in names:
        build_one(n, args.out, install, args.verbose)


if __name__ == "__main__":
    main()
