"""產物管理：時間戳備份、尺寸比較、清理。

對應 mp_Make-Tools 的 build/<name>_<時間>.bin 行為，差別是這裡多寫一份 .json 側錄
（板子 / git / IDF / sha256），OTA 回滾測試時用得上。
"""
from __future__ import annotations

import datetime as _dt
import hashlib
import json
import shutil
from pathlib import Path

from . import env


def stamp() -> str:
    return _dt.datetime.now().strftime("%Y%m%d_%H%M%S")


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def archive(cfg: dict, board: str, bin_path: Path, meta: dict | None = None) -> Path:
    """把 build 出來的 bin 複製到 artifacts/<board>_<時間>.bin，並寫 .json 側錄。"""
    out_dir = env.artifacts_dir(cfg)
    out_dir.mkdir(parents=True, exist_ok=True)

    target = out_dir / f"{board}_{stamp()}.bin"
    shutil.copy2(bin_path, target)

    info = {
        "board": board,
        "file": target.name,
        "size": target.stat().st_size,
        "sha256": sha256(target),
        "built_at": _dt.datetime.now().isoformat(timespec="seconds"),
        "git": env.git_rev(),
        "git_dirty": env.git_dirty(),
        **(meta or {}),
    }
    target.with_suffix(".json").write_text(json.dumps(info, ensure_ascii=False, indent=2) + "\n")
    return target


def list_outputs(cfg: dict, board: str | None = None) -> list[dict]:
    out_dir = env.artifacts_dir(cfg)
    if not out_dir.exists():
        return []
    rows = []
    for js in sorted(out_dir.glob("*.json"), reverse=True):
        try:
            info = json.loads(js.read_text())
        except json.JSONDecodeError:
            continue
        if board and info.get("board") != board:
            continue
        bin_path = out_dir / info.get("file", "")
        info["_exists"] = bin_path.exists()
        info["_path"] = bin_path
        rows.append(info)
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
    keep = keep if keep is not None else int((cfg.get("build") or {}).get("keep") or 10)
    removed: list[Path] = []
    boards = {i.get("board") for i in list_outputs(cfg)} if not board else [board]
    for b in sorted(x for x in boards if x):
        rows = list_outputs(cfg, b)          # 已按時間倒序
        for info in rows[keep:]:
            p: Path = info["_path"]
            for f in (p, p.with_suffix(".json")):
                if f.exists():
                    f.unlink()
                    removed.append(f)
    return removed


def latest(cfg: dict, board: str) -> Path | None:
    for info in list_outputs(cfg, board):
        if info.get("_exists"):
            return info["_path"]
    return None
