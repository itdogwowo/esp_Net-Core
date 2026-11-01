"""建置 / 燒錄 / 監看 / 清理。

和 tools/board.sh 的分工：board.sh 現在只是這支的殼（一個實作，不會兩邊漂移）。
"""
from __future__ import annotations

import re
import shutil
import time
from pathlib import Path

from . import artifacts, doctor, env

SIZE_RE = re.compile(
    r"(\S+\.bin)\s+binary size 0x([0-9a-fA-F]+) bytes.*?0x([0-9a-fA-F]+) bytes \((\d+)%\) free"
)


# ───────────────────────── 前置 ─────────────────────────

def _current_target() -> str | None:
    cfg = env.FIRMWARE / "sdkconfig"
    if not cfg.exists():
        return None
    m = re.search(r'^CONFIG_IDF_TARGET="([^"]+)"', cfg.read_text(), re.MULTILINE)
    return m.group(1) if m else None


def _idf_args(d: Path, b: dict, minimal: bool) -> list[str]:
    args = [f'-DPORT={env.port_name(d)}', f'-DIDF_TARGET={b["chip"]}']
    if minimal:
        args.append("-DNETCORE_MINIMAL=1")
    return args


def _prepare_target(chip: str, idf: Path, log: Path | None, args: list[str]) -> None:
    """sdkconfig 記的是上一次的 target；不符就重設（等於 rm sdkconfig + build）。"""
    now = _current_target()
    if now == chip or now is None:
        return
    print(f"→ target 由 {now} 改成 {chip}：重設 sdkconfig（來源 = defaults + 板子覆蓋）")
    cmd = (f'{env.export_prefix(idf)} && cd "{env.FIRMWARE}" && '
           f'idf.py -B build {" ".join(args)} set-target {chip}')
    env.bash(cmd, log=log)


def build(cfg: dict, board_query: str | None, *, minimal: bool = False,
          jobs: int | None = None, clean: bool = False, fresh: bool = False,
          do_doctor: bool = True, do_archive: bool | None = None) -> int:
    d, b, chip, chip_dir = env.resolve_board(board_query)
    board = b.get("name", d.name)
    idf = env.idf_path_for(chip, cfg)

    if do_doctor:
        checks = doctor.run_checks(cfg, board_query, quick=True)
        fails = [c for c in checks if c.status == doctor.FAIL]
        if fails:
            doctor.report(checks, title=f"環境健檢（{board}）")
            print("\n先修上面的問題再 build；--no-doctor 可跳過。")
            return 1

    if fresh:
        for p in (env.FIRMWARE / "sdkconfig",):
            if p.exists():
                p.unlink()
                print(f"→ 刪除 {p.relative_to(env.ROOT)}（重新套用 defaults）")
    if fresh or clean:
        bd = env.FIRMWARE / "build"
        if bd.exists():
            shutil.rmtree(bd)
            print(f"→ 清掉 {bd.relative_to(env.ROOT)}")

    args = _idf_args(d, b, minimal)
    out_dir = env.artifacts_dir(cfg)
    ts = artifacts.stamp()
    log = out_dir / "logs" / f"{board}_{ts}{'_minimal' if minimal else ''}.log"

    _prepare_target(chip, idf, log, args)

    jobs = jobs if jobs is not None else (cfg.get("build") or {}).get("jobs")
    jobs_env = f"CMAKE_BUILD_PARALLEL_LEVEL={int(jobs)} " if jobs else ""

    print(f"→ 建置 {board}（{chip}，IDF {env.idf_version(idf) or '?'}"
          f"{'，minimal' if minimal else ''}）")
    t0 = time.monotonic()
    cmd = (f'{env.export_prefix(idf)} && cd "{env.FIRMWARE}" && '
           f'{jobs_env}idf.py -B build {" ".join(args)} build')
    rc = env.bash(cmd, log=log)
    dur = time.monotonic() - t0

    if rc != 0:
        print(f"\n✘ 建置失敗（{dur:.0f}s）")
        print(f"  日誌：{log.relative_to(env.ROOT)}")
        tail = log.read_text(errors="replace").splitlines()[-25:] if log.exists() else []
        for ln in tail:
            if "error:" in ln.lower() or "FAILED" in ln:
                print(f"  {ln.strip()}")
        return rc

    # 尺寸與產物
    text = log.read_text(errors="replace") if log.exists() else ""
    m = SIZE_RE.search(text)
    bin_path = env.FIRMWARE / "build" / "esp_net_core.bin"
    size = int(m.group(2), 16) if m else (bin_path.stat().st_size if bin_path.exists() else 0)
    part = int(m.group(3), 16) if m else 0
    free_pct = int(m.group(4)) if m else 0

    print(f"\n✔ 建置完成  {board}  ({dur:.0f}s)")
    print(f"  image   {env.human(size)}  (0x{size:x})" + (f"，分區剩 {free_pct}%" if part else ""))

    if do_archive is None:
        do_archive = bool((cfg.get("build") or {}).get("archive", True))

    if do_archive and bin_path.exists():
        prev = artifacts.previous_size(cfg, board)
        out = artifacts.archive(cfg, board, bin_path, {
            "chip": chip, "target": b["chip"], "minimal": minimal,
            "profiles": b.get("profiles") or [],
            "idf": env.idf_version(idf),
        })
        delta = f"，{size - prev:+d} B vs 上一顆" if prev else ""
        print(f"  產物    {out.relative_to(env.ROOT)}{delta}")
        try:
            artifacts.prune(cfg, board)
        except OSError as e:
            print(f"  （清理舊產物失敗：{e}）")

    if log.exists():
        print(f"  日誌    {log.relative_to(env.ROOT)}")
    return 0


# ───────────────────────── 燒錄 / 監看 ─────────────────────────

def flash(cfg: dict, board_query: str | None, *, port: str | None = None,
          mode: str = "app", minimal: bool = False, no_build: bool = False,
          dry_run: bool = False) -> int:
    d, b, chip, _ = env.resolve_board(board_query)
    board = b.get("name", d.name)
    idf = env.idf_path_for(chip, cfg)

    if not no_build:
        rc = build(cfg, board_query, minimal=minimal, do_doctor=False)
        if rc != 0:
            return rc

    p = env.pick_port(cfg, port)
    if not p:
        print("✘ 找不到序列埠；用 --port /dev/cu.usbmodemXXXX 指定")
        return 1

    # 防止「拿 A 板的 image 燒 B 板」
    now = _current_target()
    if now and now != b["chip"]:
        print(f"✘ firmware/sdkconfig 的 target 是 {now}，不是 {b['chip']}；先 build 這塊板子")
        return 1

    args = _idf_args(d, b, minimal)
    cmd_map = {
        "app": "app-flash",
        "full": "flash",
        "erase": "erase-flash flash",
    }
    if mode not in cmd_map:
        print(f"✘ 未知的燒錄模式：{mode}")
        return 1

    cmd = (f'{env.export_prefix(idf)} && cd "{env.FIRMWARE}" && '
           f'idf.py -B build {" ".join(args)} -p "{p}" {cmd_map[mode]}')
    if dry_run:
        print(f"→ 會執行（dry-run，{board} → {p}）：\n  {cmd}")
        return 0
    print(f"→ {mode} 到 {p}（{board}）")
    return env.bash(cmd)


def monitor(cfg: dict, board_query: str | None, port: str | None = None,
            dry_run: bool = False) -> int:
    d, b, chip, _ = env.resolve_board(board_query)
    idf = env.idf_path_for(chip, cfg)
    p = env.pick_port(cfg, port)
    if not p:
        print("✘ 找不到序列埠；用 --port 指定")
        return 1
    cmd = f'{env.export_prefix(idf)} && cd "{env.FIRMWARE}" && idf.py -p "{p}" monitor'
    if dry_run:
        print(f"→ 會執行（dry-run）：\n  {cmd}")
        return 0
    print(f"→ {p}（Ctrl+] 離開）")
    return env.bash(cmd)


# ───────────────────────── 清理 / 資訊 ─────────────────────────

def clean(cfg: dict, full: bool = False) -> int:
    targets = [env.FIRMWARE / "build", env.FIRMWARE / "sdkconfig", env.FIRMWARE / "sdkconfig.old"]
    if full:
        targets += [env.FIRMWARE / "managed_components", env.FIRMWARE / "dependencies.lock"]
    for p in targets:
        if p.exists():
            if p.is_dir():
                shutil.rmtree(p)
            else:
                p.unlink()
            print(f"→ 刪除 {p.relative_to(env.ROOT)}")
        else:
            print(f"  （沒有 {p.relative_to(env.ROOT)}）")
    return 0


def show_config(cfg: dict, board_query: str | None) -> int:
    import board as boardmod   # tools/board.py

    d, b, chip, _ = env.resolve_board(board_query)
    merged = boardmod.effective_config(d)
    print(f"板子      {b.get('name')}  ({chip})")
    print(f"目錄      {env.port_name(d)}")
    print(f"IDF       {env.idf_path_for(chip, cfg)}")
    print(f"設定檔    {cfg.get('_path', '（預設值）')}")
    print(f"profiles  {', '.join(b.get('profiles') or []) or '—'}")
    print(f"驗證日    {b.get('verified') or '（未驗證）'}")
    print("\nboard.json：")
    for k in ("flash", "psram", "cpu_mhz", "console", "features"):
        print(f"  {k:<10} {b.get(k)}")

    interesting = [k for k in merged if k.startswith((
        "ESPTOOLPY_FLASHSIZE", "ESPTOOLPY_FLASHFREQ", "ESPTOOLPY_FLASHMODE",
        "SPIRAM", "ESP_CONSOLE", "ESP_DEFAULT_CPU_FREQ", "ETH_", "LWIP_TCP_MSS",
        "HTTPD_WS"))]
    print("\n有效 Kconfig（合併後）：")
    for k in sorted(interesting):
        val = merged[k]
        print(f"  CONFIG_{k}={val if val is not None else 'n'}")
    return 0
