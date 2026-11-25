"""tenv 命令列。薄殼：解析 → 探測／讀紀錄 → plan.py 判斷 → 印。

所有「判斷」都在 plan.py，所有「事實」都在 probe.py；這裏只做 I/O 與輸入驗證。
"""
from __future__ import annotations

import argparse
import json
import shutil
import sys
from pathlib import Path

from . import TenvError, load_policy, __version__
from . import plan as planmod
from . import probe, state

TOOLS = Path(__file__).resolve().parents[1]
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))
from ncbuild import env as ncenv  # noqa: E402


# ───────────────────────── 共用 ─────────────────────────

def _resolve(args) -> tuple[Path, Path]:
    repo = state.repo_root()
    return repo, state.resolve_state_dir(repo, getattr(args, "state_dir", None))


def _print_json(obj) -> None:
    print(json.dumps(obj, ensure_ascii=False, indent=2))


def _policy() -> dict:
    return load_policy()


def _load_machine(d: Path, host: dict) -> dict:
    m = state.load_machine(d)
    known = (m.get("machine") or {}).get("id")
    current = state.make_machine_id(host)
    if known and known != current:
        raise TenvError(
            "這個狀態目錄屬於另一台機（紀錄 id={}，本機 id={}）。\n"
            "  預設每台機各自一份；如果你把 home／repo 同步了，請用 "
            "--state-dir ~/.esp_net_core/testenv 指去本機專用目錄。\n"
            "  確定要覆蓋這份紀錄才用：python3 tools/tenv.py init --force".format(known, current)
        )
    # hostname／OS 版本會變；id 不變，記錄順手更新（下次 set/init 才落盤）。
    m.setdefault("machine", {})
    m["machine"].update({
        "hostname": host.get("hostname") or m["machine"].get("hostname") or "",
        "os": host.get("os") or m["machine"].get("os") or "",
        "arch": host.get("arch") or m["machine"].get("arch") or "",
    })
    return m


def _command(args) -> tuple[Path, Path, dict, dict]:
    repo, d = _resolve(args)
    host = probe.host_facts()
    m = _load_machine(d, host)
    return repo, d, m, _policy()


def _fresh(args) -> tuple[dict, dict | None, dict]:
    """探測（或讀 cache）→ (facts, last_before, plan)。"""
    repo, d, m, policy = _command(args)
    cached = bool(getattr(args, "cached", False))
    if cached:
        facts = state.load_probe(d)
        if not facts:
            raise TenvError("還沒有 probe 紀錄；先跑 `python3 tools/tenv.py plan`（唔加 --cached）")
        last = None
        plan_obj = planmod.build_plan(facts, m, policy, last)
        return facts, m, plan_obj
    last = state.load_probe(d)
    facts = probe.collect(repo, d, m, policy)
    state.save_probe(d, facts)
    plan_obj = planmod.build_plan(facts, m, policy, last)
    return facts, m, plan_obj


# ───────────────────────── 指令 ─────────────────────────

def cmd_init(args) -> int:
    repo, d = _resolve(args)
    host = probe.host_facts()
    mfile = state.machine_path(d)
    if mfile.exists() and not args.force:
        raise TenvError(
            f"這台機已經有紀錄：{mfile}\n"
            f"  直接睇：python3 tools/tenv.py brief\n"
            f"  要重建（會備份舊檔）：python3 tools/tenv.py init --force"
        )
    state.ensure_state_dir(d)
    if mfile.exists():
        backup = mfile.with_name(f"machine.json.bak-{state.now_iso().replace(':', '')}")
        shutil.copy2(mfile, backup)
        print(f"舊紀錄已備份：{backup}")
    m = state.default_machine(host, args.label)
    state.save_machine(d, m)
    (d / "README.md").write_text(state.state_readme(repo), encoding="utf-8")
    policy = load_policy()
    facts = probe.collect(repo, d, m, policy)
    state.save_probe(d, facts)
    plan_obj = planmod.build_plan(facts, m, policy, None)
    print(planmod.render_text(plan_obj))
    print("")
    print(f"紀錄位置：{mfile}（唔進 git）")
    print("下一步：接板後 `python3 tools/tenv.py bind <board> --project <project>`，"
          "或者 `tenv set board <board>` 先綁定。")
    return 0


def cmd_detect(args) -> int:
    repo, d, m, policy = _command(args)
    facts = probe.collect(repo, d, m, policy)
    state.save_probe(d, facts)
    if args.json:
        _print_json(facts)
    else:
        print(planmod.render_facts(facts))
        print("→ 看測試計劃：python3 tools/tenv.py plan")
    return 0


def cmd_plan(args) -> int:
    facts, _m, plan_obj = _fresh(args)
    if args.json:
        _print_json(plan_obj)
    else:
        print(planmod.render_text(plan_obj, verbose=args.verbose))
    return 0


def cmd_brief(args) -> int:
    _facts, _m, plan_obj = _fresh(args)
    if args.json:
        _print_json(plan_obj)
    else:
        print(planmod.render_brief(plan_obj))
    return 0


def cmd_show(args) -> int:
    repo, d, m, policy = _command(args)
    facts = state.load_probe(d)
    if not facts:
        raise TenvError("還沒有 probe 紀錄；先跑 `python3 tools/tenv.py plan`")
    plan_obj = planmod.build_plan(facts, m, policy, None)
    if args.json:
        _print_json({"machine": m, "plan": plan_obj})
        return 0
    mach = m.get("machine") or {}
    b = m.get("board") or {}
    print(f"機器 {mach.get('label')}（{mach.get('id')}）· 角色 {mach.get('role') or '—'}")
    print(f"意圖 board={b.get('expected') or '—'} cid={b.get('expected_cid') or '—'} "
          f"project={b.get('project') or '—'} absent_ok={bool(b.get('absent_ok'))}")
    if (m.get("lanes") or {}).get("disabled"):
        print(f"停用 lane：{', '.join(m['lanes']['disabled'])}")
    print("")
    print(planmod.render_text(plan_obj, verbose=args.verbose))
    print("")
    print("（以上係上次 probe 的 cache；對照現實用 `tenv verify`，重新探測用 `tenv plan`。）")
    return 0


def cmd_stamp(args) -> int:
    facts, _m, _plan = _fresh(args)
    if args.json:
        _print_json(planmod.stamp_dict(facts))
    else:
        print(planmod.render_stamp(facts))
    return 0


def cmd_verify(args) -> int:
    repo, d, m, policy = _command(args)
    last = state.load_probe(d)
    if not last:
        raise TenvError("還沒有 probe 紀錄；先跑 `python3 tools/tenv.py plan`")
    facts = probe.collect(repo, d, m, policy)
    plan_obj = planmod.build_plan(facts, m, policy, last)
    drift = plan_obj.get("drift") or []
    errors = list(facts.get("errors") or [])
    bad = [w for w in plan_obj.get("warnings") or []
           if w.get("code") in ("idf_bad", "idf_missing")]
    if args.json:
        _print_json({"drift": drift, "errors": errors, "problems": bad})
    else:
        if not drift and not errors:
            print("✔ 紀錄同現實一致（IDF、板、repo 都冇變）。")
        for ditem in drift:
            print(f"↻ {ditem['text']}")
        for e in errors:
            print(f"✘ 探測錯誤：{e}")
        for w in bad:
            print(f"✘ {w['text']}")
        if drift or errors or bad:
            print("→ 接受現況就跑 `tenv plan`（會更新 last-probe）；未更新之前，"
                  "唔好引用舊紀錄的 IDF／image。")
    return 2 if (drift or errors or bad) else 0


def _normalize_cid(value: str) -> str:
    s = str(value).strip()
    if s.lower().startswith("0x"):
        s = s[2:]
    try:
        n = int(s, 16)
    except ValueError:
        raise TenvError(f"cid 要 4 位 16 進位（例：2B4C）；收到 {value!r}")
    if not 0 <= n <= 0xFFFF:
        raise TenvError(f"cid 超出 16 bit：{value!r}")
    return f"{n:04X}"


def _bool(value: str) -> bool:
    s = str(value).strip().lower()
    if s in ("1", "true", "yes", "y", "on"):
        return True
    if s in ("0", "false", "no", "n", "off"):
        return False
    raise TenvError(f"要 true／false；收到 {value!r}")


def cmd_set(args) -> int:
    _repo, d, m, _policy = _command(args)
    key, value = args.key.strip().lower(), args.value
    mach = m.setdefault("machine", {})
    b = m.setdefault("board", {})

    if key == "label":
        if not value.strip():
            raise TenvError("label 唔可以空白")
        mach["label"] = value.strip()
    elif key == "role":
        mach["role"] = value.strip() or "dev"
    elif key == "notes":
        m["notes"] = value
    elif key == "board":
        if value.strip() in ("", "-", "none"):
            b.update({"expected": None, "expected_cid": None, "project": None})
        else:
            meta = probe.board_meta(value.strip())
            if not meta.get("valid"):
                raise TenvError(meta.get("error") or f"找不到板子 {value!r}")
            old = b.get("expected")
            b["expected"] = meta["board"]
            if old != meta["board"]:
                b["expected_cid"] = None       # 換板就不要留舊板的 cid
                if b.get("project") not in (meta.get("projects") or []):
                    b["project"] = meta.get("default_project")
            if not b.get("project"):
                b["project"] = meta.get("default_project")
    elif key == "cid":
        b["expected_cid"] = None if value.strip() in ("", "-", "none") else _normalize_cid(value)
    elif key == "project":
        if value.strip() in ("", "-", "none"):
            b["project"] = None
        else:
            meta = probe.board_meta(b.get("expected"))
            if not meta.get("valid"):
                raise TenvError("先 set board，才可以 set project")
            if value.strip() not in (meta.get("projects") or []):
                raise TenvError(f"{meta['board']} 冇 project {value!r}；有：{', '.join(meta['projects'])}")
            b["project"] = value.strip()
    elif key == "idf":
        if value.strip() in ("", "-", "none"):
            m["idf"]["pin"] = None
        else:
            p = Path(value).expanduser().resolve()
            if not ncenv.looks_like_idf(p):
                raise TenvError(f"不是 ESP-IDF（找不到 tools/idf.py）：{p}")
            m["idf"]["pin"] = str(p)
    elif key in ("absent-ok", "absent_ok"):
        b["absent_ok"] = _bool(value)
    else:
        raise TenvError(
            "可設定的 key：label／role／notes／board／cid／project／idf／absent-ok\n"
            "  lane 開關用 `tenv lane disable <lane>` / `enable`"
        )

    state.save_machine(d, m)
    print(f"✔ {key} = {value!r}（已寫入 {state.machine_path(d)}）")
    print("→ 重新探測：python3 tools/tenv.py plan")
    return 0


def cmd_lane(args) -> int:
    _repo, d, m, _policy = _command(args)
    lanes = m.setdefault("lanes", {}).setdefault("disabled", [])
    if args.action == "list":
        for lid in planmod.LANE_ORDER:
            mark = "– 停用" if lid in lanes else "✔ 啟用"
            meta = planmod.LANE_META[lid]
            print(f"{mark}  {lid:<12} {meta['title']}（需要：{'、'.join(meta['requires'])}）")
        return 0
    lid = args.lane
    if lid not in planmod.LANE_ORDER:
        raise TenvError(f"未知 lane {lid!r}；可用：{', '.join(planmod.LANE_ORDER)}")
    if args.action == "disable":
        if lid not in lanes:
            lanes.append(lid)
        print(f"✔ 已停用 lane {lid}")
    else:
        if lid in lanes:
            lanes.remove(lid)
        print(f"✔ 已啟用 lane {lid}")
    state.save_machine(d, m)
    return 0


def cmd_bind(args) -> int:
    repo, d, m, policy = _command(args)
    facts = probe.collect(repo, d, m, policy)
    state.save_probe(d, facts)
    board = facts.get("board") or {}
    if board.get("state") != "ready":
        msg = "；".join(board.get("errors") or []) or "冇 NC4 回覆"
        raise TenvError(f"讀唔到板，bind 唔到（{msg}）。接板／關掉佔住 port 的程式再試。")
    meta = probe.board_meta(args.board)
    if not meta.get("valid"):
        raise TenvError(meta.get("error") or f"找不到板子 {args.board!r}")
    chip = (board.get("sys") or {}).get("chip")
    if chip and meta.get("chip") and chip != meta["chip"]:
        raise TenvError(f"板上 chip={chip}，但 {meta['board']} 是 {meta['chip']}；"
                        f"接錯板或 board 參數寫錯。")
    project = args.project or meta.get("default_project")
    if project and project not in (meta.get("projects") or []):
        raise TenvError(f"{meta['board']} 冇 project {project!r}；有：{', '.join(meta['projects'])}")
    if args.cid == "auto":
        cid = board.get("cid_hex")
        if not cid:
            raise TenvError("板上冇回 cid；用 --cid <4 hex> 指定")
    else:
        cid = _normalize_cid(args.cid)
    m["board"] = {
        "expected": meta["board"],
        "expected_cid": cid,
        "project": project,
        "absent_ok": bool((m.get("board") or {}).get("absent_ok")),
    }
    state.save_machine(d, m)
    print(f"✔ 已綁定：board={meta['board']} cid={cid} project={project or '—'}")
    print(f"  （{board.get('port')}；fw={(board.get('sys') or {}).get('fw')}；"
          f"img={facts.get('image_kind')}）")
    print("→ 以後 `tenv brief` 會用這個做預期；接錯板會出 board_cid warning。")
    return 0


def cmd_where(args) -> int:
    repo, d = _resolve(args)
    print(f"repo        {repo}")
    print(f"state dir   {d}")
    print(f"machine     {state.machine_path(d)}"
          + ("（存在）" if state.machine_path(d).exists() else "（未建立）"))
    print(f"last-probe  {state.probe_path(d)}"
          + ("（存在）" if state.probe_path(d).exists() else "（未建立）"))
    print(f"policy file {Path(__file__).resolve().parent / 'policy.json'}")
    return 0


def cmd_selftest(_args) -> int:
    from . import selftest
    return selftest.run()


# ───────────────────────── 入口 ─────────────────────────

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="tenv.py",
        description="測試環境（每台機唔同）：探測 → 對照政策 → 講邊條 lane 跑得。",
    )
    p.add_argument("--version", action="version", version=f"tenv {__version__}")
    sub = p.add_subparsers(dest="cmd", required=True)

    def add(name: str, help_text: str, fn):
        sp = sub.add_parser(name, help=help_text)
        sp.add_argument("--state-dir", help="覆寫 state dir（預設 <repo>/.testenv）")
        sp.set_defaults(fn=fn)
        return sp

    sp = add("init", "在這台機建立本機紀錄（machine.json）", cmd_init)
    sp.add_argument("--label", help="這台機的名字（預設 os-arch-hostname）")
    sp.add_argument("--force", action="store_true", help="覆蓋舊紀錄（會先備份）")

    sp = add("detect", "探測一次並寫入 last-probe.json", cmd_detect)
    sp.add_argument("--json", action="store_true", help="輸出 JSON")

    sp = add("plan", "探測＋判斷：邊條 lane 跑得、為咩", cmd_plan)
    sp.add_argument("--json", action="store_true", help="輸出 JSON")
    sp.add_argument("--cached", action="store_true", help="用上次 probe，不重新探測")
    sp.add_argument("-v", "--verbose", action="store_true", help="顯示每個 lane 的前置與文件")

    sp = add("brief", "每次開工的短版摘要（Markdown）", cmd_brief)
    sp.add_argument("--json", action="store_true", help="輸出 JSON（同 plan --json）")
    sp.add_argument("--cached", action="store_true", help="用上次 probe")

    sp = add("show", "只看已記錄的內容（唔探測）", cmd_show)
    sp.add_argument("--json", action="store_true", help="輸出 machine＋plan JSON")
    sp.add_argument("-v", "--verbose", action="store_true")

    sp = add("stamp", "印一行環境戳記，貼入測試紀錄", cmd_stamp)
    sp.add_argument("--json", action="store_true")
    sp.add_argument("--cached", action="store_true")

    sp = add("verify", "紀錄同現實有冇漂移（唔寫檔）", cmd_verify)
    sp.add_argument("--json", action="store_true")

    sp = add("set", "改本機意圖（label／board／cid／project／idf／absent-ok）", cmd_set)
    sp.add_argument("key")
    sp.add_argument("value")

    sp = add("lane", "啟用／停用一條 lane（機器層）", cmd_lane)
    sp.add_argument("action", choices=["list", "disable", "enable"])
    sp.add_argument("lane", nargs="?", help="lane id（list 時可省）")

    sp = add("bind", "用現在接住的板，綁定本機預期（board＋cid＋project）", cmd_bind)
    sp.add_argument("board", help="硬體板名（例：ESP32-S3-Zero）")
    sp.add_argument("--project", help="project 名（預設用該板的 default project）")
    sp.add_argument("--cid", default="auto", help="4 位 hex，或 auto（預設：板上 cid）")

    add("where", "印出紀錄／政策檔位置", cmd_where)
    add("selftest", "用假事實跑盡 plan 的分支（不需要板）", cmd_selftest)
    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return args.fn(args)
    except TenvError as e:
        print(f"tenv: {e}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130
