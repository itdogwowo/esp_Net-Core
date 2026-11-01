"""命令列介面。

用法（放在 repo 根目錄，或 tools/make.py）：

    python3 make.py                      # 用 make_config.json 的 default_board
    python3 make.py p4                   # 建置某塊板子（= build p4）
    python3 make.py build s3 --minimal
    python3 make.py doctor
    python3 make.py flash p4 --port /dev/cu.usbmodem1234
    python3 make.py outputs --prune 3
"""
from __future__ import annotations

import argparse
import sys

from . import __version__, artifacts, builder, doctor, env

COMMANDS = {"build", "flash", "monitor", "doctor", "clean", "outputs",
            "config", "boards", "list", "check", "ports", "env"}


def _add_common(p: argparse.ArgumentParser) -> None:
    p.add_argument("board", nargs="?", help="板子（目錄名 / board.json name / 晶片名），預設看設定檔")


def _board_of(cfg: dict, args: argparse.Namespace) -> str | None:
    return getattr(args, "board", None) or (cfg.get("default_board") or None)


def cmd_build(cfg: dict, args: argparse.Namespace) -> int:
    return builder.build(
        cfg, _board_of(cfg, args),
        minimal=args.minimal, jobs=args.jobs,
        clean=args.clean, fresh=args.fresh,
        do_doctor=not args.no_doctor,
        do_archive=False if args.no_archive else None,
    )


def cmd_flash(cfg: dict, args: argparse.Namespace) -> int:
    mode = "erase" if args.erase else ("full" if args.full else "app")
    return builder.flash(cfg, _board_of(cfg, args), port=args.port, mode=mode,
                         minimal=args.minimal, no_build=args.no_build,
                         dry_run=args.dry_run)


def cmd_monitor(cfg: dict, args: argparse.Namespace) -> int:
    return builder.monitor(cfg, _board_of(cfg, args), port=args.port,
                           dry_run=getattr(args, "dry_run", False))


def cmd_doctor(cfg: dict, args: argparse.Namespace) -> int:
    checks = doctor.run_checks(cfg, _board_of(cfg, args), quick=args.quick)
    return doctor.report(checks)


def cmd_clean(cfg: dict, args: argparse.Namespace) -> int:
    return builder.clean(cfg, full=args.all)


def cmd_outputs(cfg: dict, args: argparse.Namespace) -> int:
    board = args.board or None
    if args.prune is not None:
        removed = artifacts.prune(cfg, board, keep=args.prune)
        print(f"清掉 {len(removed)} 個檔")
        return 0

    rows = artifacts.list_outputs(cfg, board)
    if not rows:
        print(f"{env.artifacts_dir(cfg).relative_to(env.ROOT)}/ 還沒有任何產物（先 build）")
        return 0

    # header 用 ASCII：CJK 字元寬度會讓 f-string 對齊失準
    print(f"{'board':<22} {'built_at':<20} {'size':>10} {'git':<9} file")
    print("-" * 92)
    for i in rows:
        size = env.human(int(i.get("size") or 0))
        built = str(i.get("built_at", "")).replace("T", " ")[:19]
        dirty = "*" if i.get("git_dirty") else " "
        print(f"{i.get('board', '?'):<22} {built:<20} {size:>10} {str(i.get('git', '?')) + dirty:<9} {i.get('file')}")
    print(f"\n{len(rows)} 顆；目錄 {env.artifacts_dir(cfg).relative_to(env.ROOT)}/")
    return 0


def cmd_config(cfg: dict, args: argparse.Namespace) -> int:
    return builder.show_config(cfg, _board_of(cfg, args))


def cmd_boards(cfg: dict, args: argparse.Namespace) -> int:
    import board as boardmod
    return boardmod.cmd_list(None)


def cmd_list(cfg: dict, args: argparse.Namespace) -> int:
    """舊 board.sh list 的行為：先看插著什麼，再看支援哪些。"""
    ports = env.serial_ports()
    print("序列埠：")
    if ports:
        for p in ports:
            rc, out = env.bash_capture(f'lsof "{p}" 2>/dev/null | awk \'NR==2{{print $1" (pid "$2")"}}\'')
            print(f"  {p:<32} {out or '（空閒）'}")
    else:
        print("  （沒有）")
    print()
    import board as boardmod
    print("支援的板子：")
    return boardmod.cmd_list(None)


def cmd_check(cfg: dict, args: argparse.Namespace) -> int:
    import board as boardmod

    class _A:
        board = args.board
    return boardmod.cmd_check(_A())


def cmd_ports(cfg: dict, args: argparse.Namespace) -> int:
    ports = env.serial_ports()
    if not ports:
        print("沒有偵測到 USB 序列埠")
        return 0
    for p in ports:
        rc, out = env.bash_capture(f'lsof "{p}" 2>/dev/null | awk \'NR==2{{print $1" (pid "$2")"}}\'')
        print(f"{p:<32} {out or '（空閒）'}")
    return 0


def cmd_env(cfg: dict, args: argparse.Namespace) -> int:
    import board as boardmod
    for d in boardmod.board_dirs():
        b = boardmod.load_board(d)
        chip = b["chip"]
        print(f"{b.get('name'):<24} {chip:<9} IDF={env.idf_path_for(chip, cfg)}")
    print(f"\nrepo      {env.ROOT}")
    print(f"firmware  {env.FIRMWARE}")
    print(f"設定檔    {cfg.get('_path', '（預設值）')}")
    print(f"Python    {sys.version.split()[0]}")
    return 0


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(
        prog="make.py",
        description="esp_Net-Core 建置工具（板子清單來自 ports/<chip>/<board>/board.json）",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""常用：
  python3 make.py p4                 建置（等於 build p4）
  python3 make.py s3 --minimal       最小映像（bring-up 用）
  python3 make.py doctor             環境健檢
  python3 make.py flash p4 --full    連分割表一起燒
  python3 make.py outputs --prune 3  只留每塊板子最近 3 顆
""")
    ap.add_argument("--version", action="version", version=f"ncbuild {__version__}")
    ap.add_argument("--config", help="指定設定檔（預設 make_config.json）")
    sub = ap.add_subparsers(dest="cmd")

    p = sub.add_parser("build", help="建置")
    _add_common(p)
    p.add_argument("--minimal", action="store_true", help="最小映像（只印心跳，bring-up 用）")
    p.add_argument("--jobs", type=int, help="平行編譯數（預設讓 IDF 決定）")
    p.add_argument("--clean", action="store_true", help="先清 build/")
    p.add_argument("--fresh", action="store_true", help="先清 build/ 與 sdkconfig（重新套用 defaults）")
    p.add_argument("--no-doctor", action="store_true", help="跳過建置前健檢")
    p.add_argument("--no-archive", action="store_true", help="不複製到 artifacts/")
    p.set_defaults(fn=cmd_build)

    p = sub.add_parser("flash", help="燒錄")
    _add_common(p)
    p.add_argument("--port", help="序列埠（預設自動偵測）")
    p.add_argument("--full", action="store_true", help="bootloader + 分割表 + app")
    p.add_argument("--erase", action="store_true", help="全清再燒")
    p.add_argument("--minimal", action="store_true")
    p.add_argument("--no-build", action="store_true", help="不先建置，直接燒現有 image")
    p.add_argument("--dry-run", action="store_true", help="只印出會執行的命令，不真的燒")
    p.set_defaults(fn=cmd_flash)

    p = sub.add_parser("monitor", help="序列埠監看")
    _add_common(p)
    p.add_argument("--port")
    p.add_argument("--dry-run", action="store_true")
    p.set_defaults(fn=cmd_monitor)

    p = sub.add_parser("doctor", help="環境健檢")
    _add_common(p)
    p.add_argument("--quick", action="store_true", help="跳過慢檢查（idf.py / esptool）")
    p.set_defaults(fn=cmd_doctor)

    p = sub.add_parser("clean", help="清掉 build/ 與 sdkconfig")
    p.add_argument("--all", action="store_true", help="連 managed_components / dependencies.lock 一起清")
    p.set_defaults(fn=cmd_clean)

    p = sub.add_parser("outputs", help="列出歷史產物")
    p.add_argument("board", nargs="?")
    p.add_argument("--prune", type=int, help="每塊板子只留 N 顆")
    p.set_defaults(fn=cmd_outputs)

    p = sub.add_parser("config", help="印出這塊板子的有效設定")
    _add_common(p)
    p.set_defaults(fn=cmd_config)

    p = sub.add_parser("check", help="驗 board.json ↔ sdkconfig（同 tools/board.py check）")
    _add_common(p)
    p.set_defaults(fn=cmd_check)

    p = sub.add_parser("boards", help="所有板子與能力")
    p.set_defaults(fn=cmd_boards)

    p = sub.add_parser("list", help="插著的板子 + 支援的板子")
    p.set_defaults(fn=cmd_list)

    p = sub.add_parser("ports", help="序列埠與佔用者")
    p.set_defaults(fn=cmd_ports)

    p = sub.add_parser("env", help="印出各板子對應的 IDF 路徑")
    p.set_defaults(fn=cmd_env)

    return ap


def main(argv: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)

    # `make.py p4` → `make.py build p4`（照 mp_Make-Tools 的直覺）
    if argv and not argv[0].startswith("-") and argv[0] not in COMMANDS:
        argv.insert(0, "build")

    ap = build_parser()
    args = ap.parse_args(argv)
    if not getattr(args, "fn", None):
        ap.print_help()
        return 0

    cfg = env.load_config(args.config)
    try:
        return args.fn(cfg, args)
    except SystemExit as e:
        if e.code not in (None, 0):
            print(f"✘ {e}")
        return int(e.code or 1)
    except KeyboardInterrupt:
        print("\n中止")
        return 130
