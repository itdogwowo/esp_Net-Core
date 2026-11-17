#!/usr/bin/env python3
"""獨立任務模組編譯工具（ncm build）

外掛模組**唔屬於 firmware 專案**：源碼放在 repo 根的 `modules/`，用呢個工具
獨立編譯；firmware 只負責「載入＋執行」。

命名（嚴格；見 docs/03_notes/naming.md §7）:
    modules/
    ├── tasks/         任務模組     → out/<name>.nct   （每個 leaf 名以 _tasks 結尾）
    │   ├── test_hello_tasks/
    │   └── demo_x_tasks/ / x_tasks/
    ├── actions/       指令模組     → out/<name>.nct   （leaf 名以 _actions 結尾，
    │                     kind=normal，build.json 的 cmds 必須非空）
    ├── effects/       效果模組     → out/<name>.nce   （leaf 名以 _effects 結尾）
    ├── lab/           實驗／量測    → out/<name>.nct   （leaf 名以 _lab 結尾；
    │                     一次性驗證用，唔係任務、唔會收納）
    ├── build/         中間產物（.wasm；gitignore，唔會同成品混在一起）
    └── out/           最終成品（只有 .nct／.nce；gitignore）

每個模組資料夾:
    <name>/            <name> 必須以 role 名結尾（tasks／actions／effects）
    ├── <name>.wat     （或 .c／.wasm）payload 源碼
    ├── build.json     打包參數（可選；可加 "cmds": [0x3xxx]）
    └── meta.json      名稱／描述／usage（可選）

用法:
    python3 tools/ncm_build.py                  # 編譯 modules/ 全部
    python3 tools/ncm_build.py test_hello_tasks # 只編一個
    python3 tools/ncm_build.py --list           # 列出（含 role／kind）
    python3 tools/ncm_build.py --dir ~/my-mods  # 外部任務根目錄（結構相同）
    python3 tools/ncm_build.py --clean          # 清空 build/ 同 out/
    python3 tools/ncm_build.py test_hello_tasks --device   # 編完裝到 /Volumes/NC-*/tasks
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

# role 目錄 → 預設 kind（ncm.py 的 kind；build.json 可覆寫）
ROLE_KIND = {"tasks": "task", "actions": "normal", "effects": "effect",
             "lab": "task"}
# kind → 副檔名
EXT_BY_KIND = {"task": ".nct", "normal": ".nct", "effect": ".nce"}

DEFAULTS = {
    "kind": None,       # None = 由 role 決定
    "fps": 1,
    "core": -1,
    "layer": 4,
    "prio": 3,
    "mem": 65536,
    "stack": 4096,
    "budget_us": 20000,
    "px": 0,
    "cmds": [],         # 外掛私有指令 ID（建議 0x3000–0x3FFF）
    "extra": [],
}


def die(msg: str) -> "NoReturn":
    print(f"ncm_build: {msg}", file=sys.stderr)
    sys.exit(1)


def has_source(d: Path) -> bool:
    return any((d / f"{d.name}{ext}").exists() for ext in (".wat", ".c", ".wasm"))


def check_name(name: str, role: str | None, where: Path):
    """leaf 名要跟 role 結尾（tasks／actions／effects），睇名即知類型。"""
    if role is not None:
        suffix = "_" + role
        if not name.endswith(suffix):
            die(f"{where}：模組名「{name}」要以「{suffix}」結尾"
                f"（例如 {name}{suffix}）")
    else:
        # flat 目錄：由後綴反推 role
        for r in ROLE_KIND:
            if name.endswith("_" + r):
                return r
        die(f"{where}：flat 模組名「{name}」要以 _tasks／_actions／_effects 結尾"
            f"（或者放入 modules/<role>/ 目錄）")


def discover(modules_dir: Path):
    """回傳 [{name, path, role, kind}]；role 目錄優先，亦接受 flat 目錄。"""
    out = []
    if not modules_dir.is_dir():
        return out
    for d in sorted(modules_dir.iterdir()):
        if not d.is_dir() or d.name.startswith(".") or d.name in ("out", "build"):
            continue
        if d.name in ROLE_KIND:
            for sub in sorted(d.iterdir()):
                if not sub.is_dir() or sub.name.startswith("."):
                    continue
                if not has_source(sub):
                    continue
                check_name(sub.name, d.name, sub)
                out.append({"name": sub.name, "path": sub,
                            "role": d.name, "kind": ROLE_KIND[d.name]})
        elif has_source(d):
            role = check_name(d.name, None, d)
            out.append({"name": d.name, "path": d, "role": role,
                        "kind": ROLE_KIND[role]})
    return out


def find_compiler(suffix: str):
    if suffix == ".wat":
        return shutil.which("wat2wasm")
    if suffix == ".c":
        if shutil.which("clang") and shutil.which("wasm-ld"):
            return shutil.which("clang")
        return None
    return None


def load_spec(task_dir: Path):
    spec = dict(DEFAULTS)
    raw = {}
    path = task_dir / "build.json"
    if path.exists():
        with open(path, "r", encoding="utf-8") as f:
            raw = json.load(f)
        if not isinstance(raw, dict):
            die(f"{path} 必須是 JSON object")
        unknown = set(raw) - set(DEFAULTS)
        if unknown:
            die(f"{path} 有未知欄位：{sorted(unknown)}")
        spec.update(raw)
    if not isinstance(spec["cmds"], list):
        die(f"{path} 的 cmds 必須是 array")
    for c in spec["cmds"]:
        if not isinstance(c, int) or not (0 <= c <= 0xFFFF):
            die(f"{path} 的 cmds 值不合法：{c!r}")
        if not (0x3000 <= c <= 0x3FFF):
            print(f"ncm_build: 警告：cmd 0x{c:04X} 不在外掛保留區 0x3000–0x3FFF"
                  f"（核心區唔可以由外掛佔用）", file=sys.stderr)
    return spec


def build_one(entry, out_dir: Path, build_dir: Path,
              install_dir: Path | None, verbose: bool) -> Path:
    name = entry["name"]
    task_dir = entry["path"]
    spec = load_spec(task_dir)
    role = entry["role"]

    kind = spec["kind"] or entry["kind"] or "task"
    ext = EXT_BY_KIND[kind]

    if role == "actions" and not spec["cmds"]:
        die(f"{task_dir}：actions 模組必須在 build.json 宣告 \"cmds\""
            f"（外掛保留區 0x3000–0x3FFF）")

    src = None
    for suffix in (".wat", ".c", ".wasm"):
        cand = task_dir / f"{name}{suffix}"
        if cand.exists():
            src = cand
            break
    if src is None:
        die(f"找不到 {name}.wat／.c／.wasm（{task_dir}）")

    build_dir.mkdir(parents=True, exist_ok=True)
    out_dir.mkdir(parents=True, exist_ok=True)

    if src.suffix == ".wasm":
        wasm = src
    elif src.suffix == ".wat":
        cc = find_compiler(".wat")
        if cc is None:
            die("編譯 .wat 需要 wat2wasm（macOS: brew install wabt）")
        wasm = build_dir / f"{name}.wasm"          # 中間產物唔放 out/
        subprocess.run([cc, str(src), "-o", str(wasm)], check=True)
    elif src.suffix == ".c":
        cc = find_compiler(".c")
        if cc is None:
            die("編譯 .c 需要 clang（wasm32 target）＋ wasm-ld（macOS: brew install lld）")
        wasm = build_dir / f"{name}.wasm"          # 中間產物唔放 out/
        subprocess.run([
            cc, "--target=wasm32", "-O2", "-nostdlib", "-Wl,--no-entry",
            "-Wl,--export=task_tick", "-Wl,--export=task_init",
            "-Wl,--export=memory", "-o", str(wasm), str(src),
        ], check=True)

    out = out_dir / f"{name}{ext}"
    cmd = [
        sys.executable, str(NCM_PY), "pack",
        "--payload", str(wasm), "--out", str(out),
        "--kind", kind,
        "--fps", str(spec["fps"]),
        "--core", str(spec["core"]),
        "--layer", str(spec["layer"]),
        "--prio", str(spec["prio"]),
        "--mem", str(spec["mem"]),
        "--stack", str(spec["stack"]),
        "--budget-us", str(spec["budget_us"]),
        "--px", str(spec["px"]),
    ]
    meta = task_dir / "meta.json"
    if meta.exists():
        cmd += ["--meta", str(meta)]
    for c in spec["cmds"]:
        cmd += ["--cmd", f"0x{c:04X}"]
    for extra in spec["extra"]:
        cmd.append(str(extra))
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)

    if verbose:
        dump = subprocess.run([sys.executable, str(NCM_PY), "dump", str(out)],
                              check=True, capture_output=True, text=True).stdout
        print(dump.rstrip())

    size = out.stat().st_size
    try:
        shown = out.relative_to(ROOT)
    except ValueError:
        shown = out
    print(f"ncm_build: {name}{ext}  {size} B  kind={kind}  →  {shown}")

    if install_dir is not None:
        install_dir.mkdir(parents=True, exist_ok=True)
        dst = install_dir / out.name
        shutil.copy2(out, dst)
        host = install_dir / f"._{out.name}"
        if host.exists():
            host.unlink()
        print(f"ncm_build: 已安裝 → {dst}")
    return out


def detect_install_dir():
    volumes = sorted(glob.glob("/Volumes/NC-*"))
    if not volumes:
        die("找不到 /Volumes/NC-*（MSC 未掛載？）")
    if len(volumes) > 1:
        die(f"找到多個 NC 磁碟：{volumes}；請用 --install DIR 指定")
    return Path(volumes[0]) / "tasks"


def main():
    ap = argparse.ArgumentParser(description="獨立編譯 ncm 外掛模組（.nct／.nce）")
    ap.add_argument("tasks", nargs="*", help="模組名（省略 = 全部）")
    ap.add_argument("--dir", type=Path, default=MODULES,
                    help="模組根目錄（預設 repo 根 modules/；可指向外部專案）")
    ap.add_argument("--list", action="store_true", help="列出可編譯的模組（含 role／kind）")
    ap.add_argument("--out", type=Path, default=None,
                    help="最終成品目錄（預設 <根>/out）")
    ap.add_argument("--build-dir", type=Path, default=None,
                    help="中間產物目錄（預設 <根>/build）")
    ap.add_argument("--clean", action="store_true", help="清空 build/ 同 out/")
    ap.add_argument("--install", type=Path, help="編完複製到指定目錄（例如 MSC 的 tasks/）")
    ap.add_argument("--device", action="store_true", help="自動安裝到 /Volumes/NC-*/tasks")
    ap.add_argument("-v", "--verbose", action="store_true", help="印出容器 dump")
    args = ap.parse_args()

    modules_dir = args.dir.resolve()
    out_dir = args.out.resolve() if args.out is not None else modules_dir / "out"
    build_dir = args.build_dir.resolve() if args.build_dir is not None else modules_dir / "build"

    entries = discover(modules_dir)
    names = [e["name"] for e in entries]
    by_name = {e["name"]: e for e in entries}

    if args.clean:
        for d in (out_dir, build_dir):
            if d.exists():
                for p in d.glob("*"):
                    if p.is_file():
                        p.unlink()
        print(f"ncm_build: 已清空 {out_dir} 同 {build_dir}")

    if args.list:
        for e in entries:
            print(f"{e['name']}\trole={e['role']}\tkind={e['kind']}\t{e['path']}")
        return

    selected = args.tasks or names
    if not selected:
        die(f"{modules_dir} 冇任何模組（modules/tasks/<name>_tasks/ 等）")

    unknown = [n for n in selected if n not in by_name]
    if unknown:
        die(f"未知模組：{unknown}（可用：{names}）")

    install = args.install
    if args.device:
        install = detect_install_dir()

    for n in selected:
        build_one(by_name[n], out_dir, build_dir, install, args.verbose)


if __name__ == "__main__":
    main()
