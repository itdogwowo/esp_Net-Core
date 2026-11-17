"""建置 / 燒錄 / 監看 / 清理。

和 tools/board.sh 的分工：board.sh 現在只是這支的殼（一個實作，不會兩邊漂移）。
"""
from __future__ import annotations

import re
import shlex
import shutil
import time
from pathlib import Path

from . import artifacts, doctor, env

SIZE_RE = re.compile(
    r"(\S+\.bin)\s+binary size 0x([0-9a-fA-F]+) bytes.*?0x([0-9a-fA-F]+) bytes \((\d+)%\) free"
)


# ───────────────────────── 前置 ─────────────────────────

def _current_target(sdkconfig: Path) -> str | None:
    if not sdkconfig.exists():
        return None
    m = re.search(r'^CONFIG_IDF_TARGET="([^"]+)"', sdkconfig.read_text(), re.MULTILINE)
    return m.group(1) if m else None


def _env_prefix(port: env.Port) -> str:
    """把兩層身分帶進 idf.py 的環境。

    IDF 6.1 的 component_get_requirements 會在**另一個 CMake context** 重新 include
    main/CMakeLists.txt，那時 -DPORT／-DPROJECT 的 cache 變數不一定見得到；
    環境變數一定繼承得到，所以 codegen 用 NC_PORT／NC_PROJECT。
    """
    return f'NC_PORT="{port.hw_rel}" NC_PROJECT="{port.pj_rel}" '


def _idf_args(port: env.Port, build: Path, minimal: bool, tests: bool = False) -> list[str]:
    args = [f'-DPORT={port.hw_rel}', f'-DPROJECT={port.pj_rel}',
            f'-DIDF_TARGET={port.chip}',
            # 每個 build dir 自己的 sdkconfig：多塊板並存時不用互相切 target。
            f'-DSDKCONFIG={shlex.quote(str(build / "sdkconfig"))}']
    if minimal:
        args.append("-DNETCORE_MINIMAL=1")
    # 明確傳：CMake 的 cache 會記住上一次的值，靠「不傳」＝靠運氣。
    # 之前的坑：`make.py build s3` 看似建 release，其實重用了 NC_ENABLE_TESTS=ON 的 cache。
    args.append("-DNC_ENABLE_TESTS=" + ("ON" if tests else "OFF"))
    return args


def _prepare_target(port: env.Port, cfg: dict, idf: Path, log: Path | None,
                    args: list[str], env_prefix: str = "") -> None:
    """這個 build dir 的 sdkconfig 記的是上一次的 target；不符就重設。"""
    build = env.build_dir(cfg, port)
    now = _current_target(build / "sdkconfig")
    if now == port.chip or now is None:
        return
    print(f"→ target 由 {now} 改成 {port.chip}：重設 {build.name}/sdkconfig")
    cmd = (f'{env.export_prefix(idf)} && cd "{env.FIRMWARE}" && '
           f'{env_prefix}idf.py -B "{build}" {" ".join(args)} set-target {port.chip}')
    env.bash(cmd, log=log)


def build(cfg: dict, board_query: str | None, *, minimal: bool = False,
          jobs: int | None = None, clean: bool = False, fresh: bool = False,
          do_doctor: bool = True, do_archive: bool | None = None,
          tests: bool = False, project: str | None = None,
          no_project: bool = False) -> int:
    port = env.resolve_board(board_query, project=project, no_project=no_project)
    d, b, chip = port.hw, port.board, port.chip
    board = port.output_name
    idf = env.idf_path_for(chip, cfg)
    build = env.build_dir(cfg, port)

    if do_doctor:
        checks = doctor.run_checks(cfg, board_query, quick=True)
        fails = [c for c in checks if c.status == doctor.FAIL]
        if fails:
            doctor.report(checks, title=f"環境健檢（{board}）")
            print("\n先修上面的問題再 build；--no-doctor 可跳過。")
            return 1

    if fresh:
        for p in (build / "sdkconfig", build / "sdkconfig.old"):
            if p.exists():
                p.unlink()
                print(f"→ 刪除 {p.relative_to(env.ROOT) if p.is_relative_to(env.ROOT) else p}（重新套用 defaults）")
    if fresh or clean:
        if build.exists():
            shutil.rmtree(build)
            print(f"→ 清掉 {build.relative_to(env.ROOT) if build.is_relative_to(env.ROOT) else build}")

    build.parent.mkdir(parents=True, exist_ok=True)
    args = _idf_args(port, build, minimal, tests)
    out_dir = env.artifacts_dir(cfg)
    ts = artifacts.stamp()
    log = out_dir / "logs" / f"{board}_{ts}{'_minimal' if minimal else ''}.log"

    _prepare_target(port, cfg, idf, log, args, _env_prefix(port))

    jobs = jobs if jobs is not None else (cfg.get("build") or {}).get("jobs")
    jobs_env = f"CMAKE_BUILD_PARALLEL_LEVEL={int(jobs)} " if jobs else ""

    print(f"→ 建置 {board}（{chip}，IDF {env.idf_version(idf) or '?'}"
          f"{'，minimal' if minimal else ''}）")
    print(f"  build   {build}")
    t0 = time.monotonic()
    cmd = (f'{env.export_prefix(idf)} && cd "{env.FIRMWARE}" && '
           f'{_env_prefix(port)}{jobs_env}idf.py -B "{build}" {" ".join(args)} build')
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
    bin_path = build / "esp_net_core.bin"
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
            "tests": tests,          # 側錄要寫清楚：兩個映像的行為差很多（測試指令／任務）
            "profiles": b.get("profiles") or [],
            "idf": env.idf_version(idf),
        })
        delta = f"，{size - prev:+d} B vs 上一顆" if prev else ""
        print(f"  產物    {out.relative_to(env.ROOT)}{delta}")
        flash_dir = artifacts.collect_flash_bins(cfg, board, build)
        if flash_dir is not None:
            print(f"  flash   {flash_dir.relative_to(env.ROOT)}/（bootloader／分割表／app／flash_args）")
        try:
            artifacts.prune(cfg, board)
        except OSError as e:
            print(f"  （清理舊產物失敗：{e}）")

    if log.exists():
        print(f"  日誌    {log.relative_to(env.ROOT)}")
    return 0


def build_all(cfg: dict, *, minimal: bool = False, jobs: int | None = None,
              clean: bool = False, fresh: bool = False,
              do_archive: bool | None = None, tests: bool = False,
              do_doctor: bool = True) -> int:
    """依序建置 ports/ 下每一塊板子（CI / 回歸用）。每塊有自己的 build dir。"""
    import board as boardmod

    dirs = boardmod.hardware_dirs()
    if not dirs:
        print("ports/hardware/ 下沒有任何 board.json")
        return 1

    print(f"→ 建置全部 {len(dirs)} 塊板子（每塊一個 build dir，唔會互相洗 target）\n")
    results: list[tuple[str, int]] = []
    for i, d in enumerate(dirs, 1):
        b = boardmod.load_board(d)
        name = b.get("name", d.name)
        print(f"── [{i}/{len(dirs)}] {name} ─────────────────────────────")
        rc = build(cfg, boardmod.hw_name(d), minimal=minimal, jobs=jobs,
                   clean=clean, fresh=fresh, do_doctor=(do_doctor and i == 1),
                   do_archive=do_archive, tests=tests)
        results.append((name, rc))
        if rc != 0:
            print(f"✘ {name} 失敗，停止（後面的板子沒建）")
            break
        print()

    print("─" * 60)
    for name, rc in results:
        print(f"{'✔' if rc == 0 else '✘'} {name}")
    failed = [n for n, rc in results if rc != 0]
    return 1 if failed else 0


# ───────────────────────── 燒錄 / 監看 ─────────────────────────

def flash(cfg: dict, board_query: str | None, *, port: str | None = None,
          mode: str = "app", minimal: bool = False, no_build: bool = False,
          dry_run: bool = False, tests: bool = False,
          project: str | None = None, no_project: bool = False) -> int:
    pt = env.resolve_board(board_query, project=project, no_project=no_project)
    b = pt.board
    board = pt.output_name
    idf = env.idf_path_for(pt.chip, cfg)
    # 唔可以叫 build：會遮蔽上面的 build() 函式，下面「先建置再燒錄」就變成
    # TypeError: 'WindowsPath' object is not callable（2026-11-17 實測）。
    bdir = env.build_dir(cfg, pt)

    if not no_build:
        rc = build(cfg, board_query, minimal=minimal, do_doctor=False, tests=tests,
                   project=project, no_project=no_project)
        if rc != 0:
            return rc

    p = env.pick_port(cfg, port)
    if not p:
        print("✘ 找不到序列埠；用 --port /dev/cu.usbmodemXXXX 指定")
        return 1

    # 防止「拿 A 板的 image 燒 B 板」
    now = _current_target(bdir / "sdkconfig")
    if now and now != b["chip"]:
        print(f"✘ {bdir}/sdkconfig 的 target 是 {now}，不是 {b['chip']}；先 build 這塊板子")
        return 1

    # ⚠️ `--tests` 一定要一路帶到**燒錄這一步**：idf.py 每次收到不同嘅
    # -DNC_ENABLE_TESTS 都會重新 configure＋重編。以前呢裡漏咗 tests，
    # 所以 `make.py flash s3 --tests` 先建測試映像、再喺 app-flash 時
    # 悄悄重編成 release 才寫入（燒上去嘅同建好嘅唔係同一顆）。
    args = _idf_args(pt, bdir, minimal, tests)
    cmd_map = {
        "app": "app-flash",
        "full": "flash",
        "erase": "erase-flash flash",
    }
    if mode not in cmd_map:
        print(f"✘ 未知的燒錄模式：{mode}")
        return 1

    cmd = (f'{env.export_prefix(idf)} && cd "{env.FIRMWARE}" && '
           f'{_env_prefix(pt)}idf.py -B "{bdir}" {" ".join(args)} -p "{p}" {cmd_map[mode]}')
    if dry_run:
        print(f"→ 會執行（dry-run，{board} → {p}）：\n  {cmd}")
        return 0
    print(f"→ {mode} 到 {p}（{board}）")
    return env.bash(cmd)


def monitor(cfg: dict, board_query: str | None, port: str | None = None,
            dry_run: bool = False, project: str | None = None,
            no_project: bool = False) -> int:
    pt = env.resolve_board(board_query, project=project, no_project=no_project)
    idf = env.idf_path_for(pt.chip, cfg)
    build = env.build_dir(cfg, pt)
    p = env.pick_port(cfg, port)
    if not p:
        print("✘ 找不到序列埠；用 --port 指定")
        return 1
    cmd = (f'{env.export_prefix(idf)} && cd "{env.FIRMWARE}" && '
           f'idf.py -B "{build}" -p "{p}" monitor')
    if dry_run:
        print(f"→ 會執行（dry-run）：\n  {cmd}")
        return 0
    print(f"→ {p}（Ctrl+] 離開）")
    return env.bash(cmd)


# ───────────────────────── 清理 / 資訊 ─────────────────────────

def clean(cfg: dict, full: bool = False) -> int:
    targets = [env.build_root(cfg)]
    # 舊版（或手動 idf.py -B build_p4）留在 firmware/ 的產物；一次清掉，
    # 之後 make.py 只會在 artifacts/build/ 之下建立。
    targets += sorted(env.FIRMWARE.glob("build*"))
    targets += [env.FIRMWARE / "sdkconfig", env.FIRMWARE / "sdkconfig.old"]
    if full:
        targets += [env.FIRMWARE / "managed_components", env.FIRMWARE / "dependencies.lock"]
    for p in targets:
        if p.exists():
            if p.is_dir():
                shutil.rmtree(p)
            else:
                p.unlink()
            try:
                shown = p.relative_to(env.ROOT)
            except ValueError:
                shown = p
            print(f"→ 刪除 {shown}")
        else:
            print(f"  （沒有 {p}）")
    return 0


def show_config(cfg: dict, board_query: str | None, *,
                project: str | None = None, no_project: bool = False) -> int:
    import board as boardmod   # tools/board.py

    pt = env.resolve_board(board_query, project=project, no_project=no_project)
    d, b, chip = pt.hw, pt.board, pt.chip
    merged = boardmod.effective_config(d)
    hw_profiles = b.get("profiles") or []
    default = boardmod.default_project_for_hardware(d)
    if pt.pj is None:
        proj = "—（純硬體，冇 project 預置）"
    else:
        tag = "default" if pt.pj == default else "非 default"
        proj = f"{pt.pj_rel}（{tag}）"
    print(f"硬體板    {b.get('name')}  ({chip})   {pt.hw_rel}")
    print(f"project   {proj}")
    print(f"輸出名    {pt.output_name}")
    print(f"IDF       {env.idf_path_for(chip, cfg)}")
    print(f"build     {env.build_dir(cfg, pt)}")
    print(f"設定檔    {cfg.get('_path', '（預設值）')}")
    print(f"profiles  hardware={', '.join(hw_profiles) or '—'}（project 冇 build 設定）")
    print(f"驗證日    {b.get('verified') or '（未驗證）'}")
    print("\nboard.json：")
    for k in ("flash", "psram", "cpu_mhz", "console", "features"):
        print(f"  {k:<10} {b.get(k)}")

    interesting = [k for k in merged if k.startswith((
        "ESPTOOLPY_FLASHSIZE", "ESPTOOLPY_FLASHFREQ", "ESPTOOLPY_FLASHMODE",
        "SPIRAM", "ESP_CONSOLE", "ESP_DEFAULT_CPU_FREQ", "ETH_", "LWIP_TCP_MSS",
        "HTTPD_WS"))]
    print("\n有效 Kconfig（合併後；project 層在 build 時再蓋上去）：")
    for k in sorted(interesting):
        val = merged[k]
        print(f"  CONFIG_{k}={val if val is not None else 'n'}")
    return 0
