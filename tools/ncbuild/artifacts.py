"""產物管理：命名、尺寸比較、清理。

命名規則（對齊 mp_Make-Tools 的 _unique_path）：
    第一次建置  → artifacts/<board>.bin
    再建一次    → 舊的那顆改名成 artifacts/<board>_<它的建置時間>.bin，新的仍叫 <board>.bin
    還是撞名    → <board>_<時間>_1.bin、_2 …

差別在「時間戳貼在誰身上」：mp_Make-Tools 是貼在**新**產物上（舊的固定叫 NAME.bin），
這裡是貼在**舊**產物上。理由是 NAME.bin 必須永遠是最新那顆 —— 否則
`ota.py artifacts/<board>.bin` 會燒到第一次建置的舊韌體，而且不會有任何提示。
"""
from __future__ import annotations

import datetime as _dt
import hashlib
import json
import shutil
from pathlib import Path

from . import env

TS_FMT = "%Y_%m_%d_%H_%M_%S"       # 與 mp_Make-Tools 相同的檔名格式


def stamp() -> str:
    return _dt.datetime.now().strftime(TS_FMT)


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _read_sidecar(bin_path: Path) -> dict:
    js = bin_path.with_suffix(".json")
    if not js.exists():
        return {}
    try:
        return json.loads(js.read_text())
    except json.JSONDecodeError:
        return {}


def _stamp_of(bin_path: Path) -> str:
    """舊產物的時間戳：優先側錄的 built_at，退回檔案 mtime。"""
    info = _read_sidecar(bin_path)
    iso = info.get("built_at")
    if iso:
        try:
            return _dt.datetime.fromisoformat(iso).strftime(TS_FMT)
        except ValueError:
            pass
    return _dt.datetime.fromtimestamp(bin_path.stat().st_mtime).strftime(TS_FMT)


def _unique(path: Path) -> Path:
    """撞名就加後綴（_1、_2…）；用於「已經帶時間戳還撞」的情況。"""
    if not path.exists():
        return path
    i = 1
    while True:
        cand = path.with_name(f"{path.stem}_{i}{path.suffix}")
        if not cand.exists():
            return cand
        i += 1


def archive(cfg: dict, board: str, bin_path: Path, meta: dict | None = None) -> Path:
    """把建置產物放到 artifacts/<board>.bin；舊的那顆自動退成帶時間戳的歷史檔。"""
    out_dir = env.artifacts_dir(cfg)
    out_dir.mkdir(parents=True, exist_ok=True)

    plain = out_dir / f"{board}.bin"
    if plain.exists():
        ts = _stamp_of(plain)
        rotated = _unique(out_dir / f"{board}_{ts}.bin")
        plain.rename(rotated)
        side = plain.with_suffix(".json")
        if side.exists():
            new_side = rotated.with_suffix(".json")
            side.rename(new_side)
            try:                       # 側錄裡的 file 要跟著改名，否則 outputs/prune 會指錯
                info = json.loads(new_side.read_text())
                info["file"] = rotated.name
                new_side.write_text(json.dumps(info, ensure_ascii=False, indent=2) + "\n")
            except (json.JSONDecodeError, OSError):
                pass

    shutil.copy2(bin_path, plain)

    info = {
        "board": board,
        "file": plain.name,
        "size": plain.stat().st_size,
        "sha256": sha256(plain),
        "built_at": _dt.datetime.now().isoformat(timespec="seconds"),
        "git": env.git_rev(),
        "git_dirty": env.git_dirty(),
        **(meta or {}),
    }
    plain.with_suffix(".json").write_text(json.dumps(info, ensure_ascii=False, indent=2) + "\n")
    return plain


def list_outputs(cfg: dict, board: str | None = None) -> list[dict]:
    out_dir = env.artifacts_dir(cfg)
    if not out_dir.exists():
        return []
    rows = []
    for js in out_dir.glob("*.json"):
        try:
            info = json.loads(js.read_text())
        except json.JSONDecodeError:
            continue
        if board and info.get("board") != board:
            continue
        bin_path = out_dir / info.get("file", js.stem + ".bin")
        info["_exists"] = bin_path.exists()
        info["_path"] = bin_path
        info["_latest"] = bin_path.name == f"{info.get('board')}.bin"
        rows.append(info)
    rows.sort(key=lambda i: str(i.get("built_at", "")), reverse=True)
    return rows


def previous_size(cfg: dict, board: str, exclude: Path | None = None) -> int | None:
    """同板子上一顆產物的尺寸（用來算這次大了多少）。"""
    for info in list_outputs(cfg, board):
        p = info["_path"]
        if exclude and p.resolve() == exclude.resolve():
            continue
        if info.get("_exists"):
            return int(info.get("size") or p.stat().st_size)
    return None


def prune(cfg: dict, board: str | None = None, keep: int | None = None) -> list[Path]:
    """每塊板子只留最新的 keep 顆（含 <board>.bin）。"""
    keep = keep if keep is not None else int((cfg.get("build") or {}).get("keep") or 10)
    removed: list[Path] = []
    boards = {i.get("board") for i in list_outputs(cfg)} if not board else [board]
    for b in sorted(x for x in boards if x):
        rows = list_outputs(cfg, b)          # 已按 built_at 倒序
        for info in rows[keep:]:
            p: Path = info["_path"]
            for f in (p, p.with_suffix(".json")):
                if f.exists():
                    f.unlink()
                    removed.append(f)
    return removed


def latest(cfg: dict, board: str) -> Path | None:
    p = env.artifacts_dir(cfg) / f"{board}.bin"
    return p if p.exists() else None
