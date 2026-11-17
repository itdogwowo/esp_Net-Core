#!/usr/bin/env python3
"""獨立任務模組編譯工具（ncm build）

外掛任務（模組）**唔屬於 firmware 專案**：源碼放在 repo 根的 `modules/`，
用呢個工具獨立編譯成 `.nct`；firmware 只負責「載入＋執行」。

用法:
    python3 tools/ncm_build.py                  # 編譯 modules/ 全部任務
    python3 tools/ncm_build.py hello_task       # 只編一個
    python3 tools/ncm_build.py --list           # 列出可編譯的任務
    python3 tools/ncm_build.py --dir ~/my-mods  # 用外部任務目錄（獨立專案）
    python3 tools/ncm_build.py --out build/ncm  # 自訂輸出目錄
    python3 tools/ncm_build.py --clean          # 清空輸出目錄
    python3 tools/ncm_build.py hello_task --device   # 編譯＋裝到 /Volumes/NC-*/tasks
    python3 tools/ncm_build.py --device         # 全部編譯＋安裝

任務目錄約定（每個任務一個資料夾）:
    modules/
    └── <name>/
        ├── <name>.wat       （或 .c／.wasm）payload 源碼
        ├── build.json       打包參數（可選；缺省用預設）
        └── meta.json        名稱／描述／usage（可選）

`build.json` 欄位（全部可選）:
    {"kind": "task", "fps": 1, "core": -1, "layer": 4, "prio": 3,
     "mem": 65536, "stack": 4096, "budget_us": 20000, "px": 0,
     "kind_flag": "normal", "extra": ["--cmd", "0x3101"]}

C 模組（未來）:
    clang --target=wasm32 -nostdlib ... 需要 wasm-ld（brew install lld）。
    現在如果 .c 存在但工具鏈唔齊，會清楚報錯，不會靜靜跳過。

輸出: 預設 `modules/out/<name>.nct`（`--dir` 時預設 `<dir>/out/`）。
"""

import argparse
import glob
import json
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MODULES = ROOT / "modules"
NCM_PY = ROOT / "tools" / "ncm.py"

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
        return shutil.which("wat2wasm")
    if suffix == ".c":
        if shutil.which("clang") and shutil.which("wasm-ld"):
            return shutil.which("clang")
        return None
    return None


def task_names(modules_dir: Path):
    names = []
    if not modules_dir.is_dir():
        return names
    for d in sorted(modules_dir.iterdir()):
        if not d.is_dir() or d.name.startswith(".") or d.name == "out":
            continue
        if any((d / f"{d.name}{ext}").exists() for ext in (".wat", ".c", ".wasm")):
            names.append(d.name)
    return names


def load_spec(task_dir: Path):
    spec = dict(DEFAULTS)
    path = task_dir / "build.json"
    if path.exists():
        with open(path, "r", encoding="utf-8") as f:
            user = json.load(f)
        if not isinstance(user, dict):
            die(f"{path} 必須是 JSON object")
        unknown = set(user) - set(DEFAULTS)
        if unknown:
            die(f"{path} 有未知欄位：{sorted(unknown)}")
        spec.update(user)
    return spec


def build_one(name: str, modules_dir: Path, out_dir: Path,
              install_dir: Path | None, verbose: bool) -> Path:
    task_dir = modules_dir / name
    spec = load_spec(task_dir)

    src = None
    for ext in (".wat", ".c", ".wasm"):
        cand = task_dir / f"{name}{ext}"
        if cand.exists():
            src = cand
            break
    if src is None:
        die(f"找不到 {name}.wat／.c／.wasm（{task_dir}）")

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
    meta = task_dir / "meta.json"
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
    try:
        shown = nct.relative_to(ROOT)
    except ValueError:
        shown = nct
    print(f"ncm_build: {name}.nct  {size} B  →  {shown}")

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
    ap = argparse.ArgumentParser(description="獨立編譯 ncm 外掛任務（.nct）")
    ap.add_argument("tasks", nargs="*", help="任務名（省略 = 全部）")
    ap.add_argument("--dir", type=Path, default=MODULES,
                    help="任務目錄（預設 repo 根的 modules/；可指向外部專案）")
    ap.add_argument("--list", action="store_true", help="列出可編譯的任務")
    ap.add_argument("--out", type=Path, default=None,
                    help="輸出目錄（預設 <任務目錄>/out）")
    ap.add_argument("--clean", action="store_true", help="清空輸出目錄")
    ap.add_argument("--install", type=Path, help="編完複製 .nct 到指定目錄（例如 MSC 的 tasks/）")
    ap.add_argument("--device", action="store_true", help="自動安裝到 /Volumes/NC-*/tasks")
    ap.add_argument("-v", "--verbose", action="store_true", help="印出容器 dump")
    args = ap.parse_args()

    modules_dir = args.dir.resolve()
    out_dir = args.out.resolve() if args.out is not None else modules_dir / "out"

    names = task_names(modules_dir)
    if args.clean:
        if out_dir.exists():
            for p in out_dir.glob("*.wasm"):
                p.unlink()
            for p in out_dir.glob("*.nct"):
                p.unlink()
        print(f"ncm_build: 已清空 {out_dir}")

    if args.list:
        for n in names:
            print(n)
        return

    selected = args.tasks or names
    if not selected:
        die(f"{modules_dir} 冇任何任務（每個任務一個資料夾＋<name>.wat）")

    unknown = [n for n in selected if n not in names]
    if unknown:
        die(f"未知任務：{unknown}（可用：{names}）")

    install = args.install
    if args.device:
        install = detect_install_dir()

    for n in selected:
        build_one(n, modules_dir, out_dir, install, args.verbose)


if __name__ == "__main__":
    main()
