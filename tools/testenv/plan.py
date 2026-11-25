"""把事實＋本機意圖＋政策，變成「邊條測試 lane 跑得、為咩」。

Lane 形狀固定（id／status／command／gate／doc），文字與 JSON 都由同一份資料出，
所以 AI 讀 JSON、人讀文字，不會有兩套講法。

狀態四種，缺一不可：
    runnable  前置都齊，可以照 command 跑。
    blocked   前置唔齊（冇板／冇 IDF／image 唔啱）。原因要具體，唔可以「未知錯誤」。
    unknown   探測不足（例如有 port 但 STATUS 冇回覆、cmds 讀唔到）——唔等於 blocked，
              因為可能只是線鬆／port 被佔，重試會變。
    disabled  machine.json 明確關掉（absent_ok／lanes.disabled）。係「設計上唔跑」。
"""
from __future__ import annotations

import re
from datetime import datetime
from pathlib import Path

from . import human_age

RUN = "runnable"
BLOCKED = "blocked"
UNKNOWN = "unknown"
DISABLED = "disabled"

STATUS_LABEL = {RUN: "可跑", BLOCKED: "暫停", UNKNOWN: "未知", DISABLED: "停用"}
STATUS_MARK = {RUN: "✔", BLOCKED: "✘", UNKNOWN: "?", DISABLED: "–"}

LANE_META = {
    "host": {
        "title": "主機測試（唔使板）",
        "doc": "docs/02_guides/testing.md",
        "requires": ["bash", "C 編譯器", "tools/hosttest/run_all.sh"],
    },
    "build": {
        "title": "建置目標映像",
        "doc": "docs/02_guides/build_and_flash.md",
        "requires": ["ESP-IDF", "machine.board.expected"],
    },
    "board": {
        "title": "板上狀態（sys／tasks／mem）",
        "doc": "docs/02_guides/testing.md",
        "requires": ["序列埠", "NC4 回覆"],
    },
    "pixel": {
        "title": "Pixel 四任務在跑",
        "doc": "docs/02_guides/testing.md",
        "requires": ["板", "pixel_effect_task／paint／encode／render 全部 level=4"],
    },
    "ota": {
        "title": "USB NC4 OTA",
        "doc": "docs/02_guides/build_and_flash.md",
        "requires": ["板", "artifacts/ 有對應 image"],
    },
    "tests": {
        "title": "測試映像指令（0xF0xx）",
        "doc": "docs/01_protocol/commands.md",
        "requires": ["板", "image 有 0xF061（NC_ENABLE_TESTS）"],
    },
    "effect-perf": {
        "title": "效果層量測（EffectPerf）",
        "doc": "docs/00_pm/effect-perf.md",
        "requires": ["板", "image 有 0xF065（EffectPerf 映像）"],
    },
    "ncm": {
        "title": "外掛任務（NCM／WAMR）",
        "doc": "docs/02_guides/testing.md",
        "requires": ["板", "image 有 ncm 段（tests/NCM 建置）"],
    },
}
LANE_ORDER = ["host", "build", "board", "pixel", "ota", "tests", "effect-perf", "ncm"]


def _lane(lane_id: str, status: str, reason: str = "", command: str = "",
          gate: str = "", warns: list[str] | None = None) -> dict:
    meta = LANE_META[lane_id]
    d = {
        "id": lane_id,
        "title": meta["title"],
        "status": status,
        "reason": reason,
        "command": command,
        "gate": gate,
        "doc": meta["doc"],
        "requires": list(meta["requires"]),
    }
    if warns:
        d["warns"] = list(warns)
    return d


def _rel(repo: Path, p: str | Path | None) -> str:
    if not p:
        return "—"
    try:
        return str(Path(p).resolve().relative_to(repo))
    except (ValueError, OSError):
        s = str(p)
        home = str(Path.home())
        return "~" + s[len(home):] if s.startswith(home) else s


def _short(p: str | None) -> str:
    if not p:
        return "—"
    s = str(p)
    home = str(Path.home())
    return "~" + s[len(home):] if s.startswith(home) else s


def _board_absent_reason(facts: dict) -> str:
    b = facts.get("board") or {}
    t = facts.get("target") or {}
    if b.get("state") == "unknown":
        errs = "; ".join(b.get("errors") or []) or "冇回覆"
        return f"有序列埠但讀唔到板：{errs}"
    exp = t.get("board")
    if exp:
        return f"未接板（expected {exp}）"
    return "未接板，而且 machine.board.expected 未設定（用例：tenv set board <board>）"


def _board_ready(facts: dict) -> bool:
    return (facts.get("board") or {}).get("state") == "ready"


def _pixel_problem(board: dict) -> tuple[str, str]:
    """回 (status, reason)。status 是 RUN／BLOCKED／UNKNOWN。"""
    tasks = board.get("tasks")
    if tasks is None:
        why = "tasks 被截斷" if board.get("tasks_truncated") else "status tasks 冇回覆／解析不到"
        return UNKNOWN, f"讀唔到任務表（{why}）"
    px = board.get("pixel") or {}
    missing = [role for role in ("effect", "paint", "encode", "render") if not px.get(role)]
    if missing:
        names = "、".join(missing)
        return BLOCKED, f"板上 image 冇呢啲任務：{names}（可能係 EffectPerf／非 pixel 映像）"
    bad = []
    for role in ("effect", "paint", "encode", "render"):
        v = px[role]
        if v.get("level") != 4 or not v.get("started") or not v.get("running"):
            bad.append(f"{v.get('name')}(level={v.get('level')}, started={v.get('started')}, running={int(bool(v.get('running')))})")
    if bad:
        return BLOCKED, ("任務未齊： " + "、".join(bad) +
                         "；換 project／config 舊了先 `fs_format` 重 seed（AGENTS 鐵律 6）")
    return RUN, ""


def _idf_build_block(facts: dict) -> str | None:
    idf = facts.get("idf") or {}
    chosen = idf.get("chosen")
    if not chosen or not chosen.get("valid"):
        return idf.get("error") or "找不到可用的 ESP-IDF（tenv plan -v 看候選；或 tenv set idf <path>）"
    return None


def _drift(facts: dict, last: dict | None) -> list[dict]:
    if not last:
        return []
    out: list[dict] = []
    old_repo, new_repo = last.get("repo") or {}, facts.get("repo") or {}
    if old_repo.get("rev") and new_repo.get("rev") and old_repo["rev"] != new_repo["rev"]:
        out.append({"code": "repo_moved",
                    "text": f"repo {old_repo['rev']} → {new_repo['rev']}"})
    old_idf, new_idf = (last.get("idf") or {}).get("chosen") or {}, (facts.get("idf") or {}).get("chosen") or {}
    if old_idf.get("version") != new_idf.get("version"):
        out.append({"code": "idf_changed",
                    "text": f"IDF {old_idf.get('version') or '?'} → {new_idf.get('version') or '?'}"})
    elif old_idf.get("path") and new_idf.get("path") and old_idf["path"] != new_idf["path"]:
        out.append({"code": "idf_path_changed",
                    "text": f"IDF 路徑 {_short(old_idf['path'])} → {_short(new_idf['path'])}"})
    old_b, new_b = last.get("board") or {}, facts.get("board") or {}
    if old_b.get("cid_hex") and new_b.get("cid_hex") and old_b["cid_hex"] != new_b["cid_hex"]:
        out.append({"code": "board_changed",
                    "text": f"板 cid {old_b['cid_hex']} → {new_b['cid_hex']}"})
    old_sys, new_sys = old_b.get("sys") or {}, new_b.get("sys") or {}
    if old_sys.get("app") and new_sys.get("app") and old_sys["app"] != new_sys["app"]:
        out.append({"code": "device_reflashed",
                    "text": (f"板上 image app {old_sys['app']} → {new_sys['app']}"
                             f"（fw {old_sys.get('fw') or '?'} → {new_sys.get('fw') or '?'}）")})
    return out


def _warnings(facts: dict, policy: dict, drift: list[dict]) -> list[dict]:
    out: list[dict] = []

    def add(level: str, code: str, text: str) -> None:
        out.append({"level": level, "code": code, "text": text})

    idf = facts.get("idf") or {}
    chosen = idf.get("chosen") or {}
    req = idf.get("required")
    if chosen.get("valid") and idf.get("matches") is False:
        add("warn", "idf_policy",
            f"IDF {chosen.get('version') or '?'} ≠ 政策 {req}（{idf.get('source') or 'policy.json'}）："
            f"同一份 code 會出唔同 image，跨機數字唔可以互相對比；記錄要寫 "
            f"idf={chosen.get('version') or '?'}。")
    elif chosen.get("valid") and idf.get("matches") is None:
        add("warn", "idf_unknown",
            f"讀唔到 IDF 版本（政策要求 {req}）；唔可以同其他機的數字比較。")
    if idf.get("error"):
        add("warn", "idf_bad", idf["error"])
    if not chosen.get("valid"):
        add("warn", "idf_missing", "找不到可用的 ESP-IDF：build／量測類 lane 會暫停。")

    valid_versions = sorted({
        str(c.get("version")) for c in (idf.get("candidates") or [])
        if c.get("valid") and c.get("version")
    })
    if len(valid_versions) > 1 and chosen.get("valid"):
        add("info", "idf_multi",
            f"本機有多個 IDF：{', '.join(valid_versions)}；今次用 {chosen.get('version')}"
            f"（{chosen.get('source')}）。")

    b = facts.get("board") or {}
    t = facts.get("target") or {}
    if b.get("state") == "ready":
        exp_cid = (t.get("expected_cid") or "").upper()
        if exp_cid and b.get("cid_hex") and exp_cid != b["cid_hex"]:
            add("warn", "board_cid",
                f"接住的板 cid={b['cid_hex']} ≠ 紀錄的 {exp_cid}；可能接錯板（要接受就 tenv set cid {b['cid_hex']}）。")
        sysd = b.get("sys") or {}
        rst = sysd.get("rst")
        if rst in (4, 5, 6):
            why = {4: "panic", 5: "中斷 WDT", 6: "task WDT"}.get(rst, "?")
            add("warn", "device_rst",
                f"板上上次 reset 係 {rst}（{why}）；先查 log／panic，唔好當成正常量測。")
        fw = sysd.get("fw") or ""
        repo_rev = (facts.get("repo") or {}).get("rev") or ""
        m = re.fullmatch(r"([0-9a-f]{7,40})(?:-dirty)?", fw)
        if m and repo_rev and not repo_rev.startswith(m.group(1)):
            add("warn", "fw_stale",
                f"板上 fw={fw} ≠ 工作區 rev={repo_rev}；可能係另一台機 build／未 OTA（量測前先對齊同一 image）。")
        caps = b.get("caps") or {}
        art = (facts.get("artifacts") or {}).get("chosen")
        if art and caps and art.get("tests") is not None and bool(art["tests"]) != bool(caps.get("test_action")):
            add("info", "image_artifact",
                f"本機最新 image tests={bool(art['tests'])}，但板上 0xF061={bool(caps.get('test_action'))}；"
                f"唔一定同一份 image，OTA 前先確認。")
    elif b.get("state") == "absent" and t.get("board") and not t.get("absent_ok"):
        add("info", "board_absent", f"未接板（expected {t['board']}）；板類 lane 會暫停，host／build 不受影響。")

    art = (facts.get("artifacts") or {}).get("chosen")
    if art and chosen.get("valid") and art.get("idf") and art["idf"] != chosen.get("version"):
        add("info", "artifact_idf",
            f"最新 image 用 {art['idf']} build，主機 IDF 係 {chosen.get('version')}；重建後 hash 會變。")

    ht = facts.get("host_tests") or {}
    if ht.get("error"):
        add("warn", "host_tests", ht["error"])
    elif ht.get("count") != ht.get("expected"):
        add("warn", "host_tests_count",
            f"run_all.sh 有 {ht.get('count')} 支，政策寫 {ht.get('expected')} 支；確認係加／減測試而唔係漏跑。")
    if ht.get("missing"):
        add("warn", "host_tests_missing", f"run_all.sh 列的測試有目錄唔存在：{', '.join(ht['missing'])}")

    st = facts.get("state") or {}
    if st.get("sync_risk"):
        add("warn", "state_sync",
            f"repo 位於 {st['sync_risk']} 之下，{st.get('shown')}/ 可能被雲端同步；"
            f"建議 --state-dir ~/.esp_net_core/testenv。")
    if st.get("gitignored") is False:
        add("warn", "state_not_ignored",
            f"{st.get('shown')}/ 冇被 gitignore；本機紀錄有機會被 commit。")

    age = _age_seconds(facts)
    max_age = ((policy.get("probe") or {}).get("max_age_days") or 7) * 86400
    if age is not None and age > max_age:
        add("warn", "probe_stale",
            f"上次探測係 {human_age(age)}前（> {int(max_age // 86400)} 日）；"
            f"先跑 `tenv plan` 重新探測，唔好靠 --cached 做判斷。")

    if facts.get("errors"):
        add("warn", "probe_errors", f"探測有 {len(facts['errors'])} 個錯誤：" + "；".join(facts["errors"]))

    if drift:
        add("info", "drift", "同上次 probe 有變化；下面的 drift 已寫入新紀錄。")

    if not t.get("board"):
        latest = ((facts.get("artifacts") or {}).get("latest") or [None])[0]
        if latest:
            add("info", "bind_hint",
                f"偵測到最新 build：{latest.get('name')}。如果就係本機常接嘅板，"
                f"用 tenv bind {latest.get('board')}"
                + (f" --project {latest['project']}" if latest.get("project") else "")
                + " 綁定（接板後會順便記 cid）。")
        else:
            add("info", "bind_hint", "machine.board.expected 未設定；接板後用 tenv bind <board> 綁定。")
    return out


def _next_steps(lanes: list[dict], facts: dict, machine: dict, warnings: list[dict]) -> list[str]:
    by = {x["id"]: x for x in lanes}
    t = facts.get("target") or {}
    out: list[str] = []
    if not t.get("board"):
        out.append("綁定本機常接嘅板：`python3 tools/tenv.py bind <board> [--project <project>]`（要接板；或 tenv set board …）")
    if by["build"]["status"] == BLOCKED and "ESP-IDF" in by["build"]["reason"]:
        out.append("裝／指定 IDF：`tenv set idf <path>` 或設 `IDF_PATH`（tenv plan -v 看所有候選）。")
    if by["board"]["status"] in (BLOCKED, UNKNOWN):
        out.append("板類 lane 未開：接板／換線／關掉佔住 port 的程式，再跑 `tenv plan`。")
    if by["pixel"]["status"] == BLOCKED and "level=" in by["pixel"]["reason"]:
        out.append("Pixel 任務冇起：換 project／config 舊了，用 `fs_format` 重 seed（AGENTS 鐵律 6）。")
    if by["tests"]["status"] == BLOCKED and "0xF061" in by["tests"]["reason"]:
        out.append("要 0xF0xx：先 build 測試映像（`make.py build <board> --tests`）再 OTA。")
    if by["effect-perf"]["status"] == BLOCKED and "0xF065" in by["effect-perf"]["reason"]:
        out.append("要 EffectPerf：`make.py build <board> --project <EffectPerf>` 後 OTA。")
    if any(w["code"] == "idf_policy" for w in warnings):
        out.append("IDF 同政策唔同：要跨機比較就三台一齊升／降；未升之前記錄寫 `idf=`（tenv stamp 會帶）。")
    return out[:4]


def build_plan(facts: dict, machine: dict, policy: dict, last: dict | None = None) -> dict:
    board = facts.get("board") or {}
    target = facts.get("target") or {}
    idf = facts.get("idf") or {}
    art = facts.get("artifacts") or {}
    ht = facts.get("host_tests") or {}
    disabled = set(((machine.get("lanes") or {}).get("disabled") or []))
    drift = _drift(facts, last)
    warnings = _warnings(facts, policy, drift)

    lanes: list[dict] = []

    # ── host ──
    host = facts.get("host") or {}
    tools = host.get("tools") or {}
    if "host" in disabled:
        lanes.append(_lane("host", DISABLED, "machine.json：lane 已停用"))
    elif not tools.get("bash"):
        lanes.append(_lane("host", BLOCKED, "冇 bash（Windows 要 Git Bash／WSL；或逐支跑 run.bat）"))
    elif not tools.get("cc"):
        lanes.append(_lane("host", BLOCKED, "冇 C 編譯器（cc／gcc／clang）"))
    elif ht.get("error") or not ht.get("count"):
        lanes.append(_lane("host", BLOCKED, ht.get("error") or "run_all.sh 找不到 TESTS 清單"))
    else:
        n = ht["count"]
        lanes.append(_lane(
            "host", RUN,
            command=f"bash {ht.get('runner') or 'tools/hosttest/run_all.sh'}",
            gate=f"{n}/{n} 支全綠（-Wall -Wextra -Werror）",
            warns=[f"政策預期 {ht.get('expected')} 支，實際 {n} 支"] if ht.get("expected") not in (None, n) else None,
        ))

    # ── build ──
    build_block = _idf_build_block(facts)
    if "build" in disabled:
        lanes.append(_lane("build", DISABLED, "machine.json：lane 已停用"))
    elif build_block:
        lanes.append(_lane("build", BLOCKED, build_block))
    elif not target.get("valid"):
        lanes.append(_lane("build", BLOCKED, target.get("error") or "machine.board.expected 未設定"))
    else:
        cmd = f"IDF_PATH={_short((idf.get('chosen') or {}).get('path'))} python3 make.py build {target.get('board')}"
        if target.get("project"):
            cmd += f" --project {target['project']}"
        lanes.append(_lane(
            "build", RUN, command=cmd,
            gate="rc=0；artifacts/ 的 .json 與 .bin sha256 一致",
        ))

    # ── board ──
    if "board" in disabled:
        lanes.append(_lane("board", DISABLED, "machine.json：lane 已停用"))
    elif board.get("state") == "ready":
        port = board.get("port")
        lanes.append(_lane(
            "board", RUN,
            command=f"python3 tools/nc4.py --port {port} status sys",
            gate="sys／tasks／mem 都有回覆；rst 唔係 4（panic）／5（int WDT）／6（task WDT）",
        ))
    elif board.get("state") == "absent" and target.get("absent_ok"):
        lanes.append(_lane("board", DISABLED, "machine.json：absent_ok=true（本機設計上唔接板）"))
    else:
        st = UNKNOWN if board.get("state") == "unknown" else BLOCKED
        lanes.append(_lane("board", st, _board_absent_reason(facts)))

    # ── pixel ──
    if "pixel" in disabled:
        lanes.append(_lane("pixel", DISABLED, "machine.json：lane 已停用"))
    elif not _board_ready(facts):
        st = UNKNOWN if board.get("state") == "unknown" else BLOCKED
        lanes.append(_lane("pixel", st, _board_absent_reason(facts)))
    else:
        st, why = _pixel_problem(board)
        port = board.get("port")
        lanes.append(_lane(
            "pixel", st, why,
            command=f"python3 tools/nc4.py --port {port} status tasks" if st == RUN else "",
            gate="pixel_effect_task／pixel_paint_task／pixel_encode_task／render_task 全部 level=4、started=1、running=1",
        ))

    # ── ota ──
    chosen_art = art.get("chosen")
    if "ota" in disabled:
        lanes.append(_lane("ota", DISABLED, "machine.json：lane 已停用"))
    elif not _board_ready(facts):
        st = UNKNOWN if board.get("state") == "unknown" else BLOCKED
        lanes.append(_lane("ota", st, _board_absent_reason(facts)))
    elif not target.get("board"):
        lanes.append(_lane("ota", BLOCKED, "machine.board.expected 未設定；唔知應該推邊塊板"))
    elif not chosen_art:
        lanes.append(_lane("ota", BLOCKED,
                           f"artifacts/ 冇 {target['board']} 的 image；先 make.py build"))
    else:
        warns = []
        caps = board.get("caps") or {}
        if chosen_art.get("tests") is not None and caps and bool(chosen_art["tests"]) != bool(caps.get("test_action")):
            warns.append(f"會由 0xF061={bool(caps.get('test_action'))} 換成 tests={bool(chosen_art['tests'])} 的 image")
        lanes.append(_lane(
            "ota", RUN,
            command=f"python3 tools/ota.py --port {board.get('port')} {_rel(Path((facts.get('repo') or {}).get('root') or '.'), chosen_art.get('bin'))}",
            gate="APPLY 後 status sys 的 app 變、rst=3；WRITE 期間唔好拔線",
            warns=warns or None,
        ))

    # ── tests / effect-perf / ncm ──
    def image_lane(lane_id: str, cap_key: str, cap_pretty: str, command: str, gate: str) -> dict:
        if lane_id in disabled:
            return _lane(lane_id, DISABLED, "machine.json：lane 已停用")
        if not _board_ready(facts):
            st = UNKNOWN if board.get("state") == "unknown" else BLOCKED
            return _lane(lane_id, st, _board_absent_reason(facts))
        caps = board.get("caps")
        if not isinstance(caps, dict):
            extra = f"（{board.get('cmds_error')}）" if board.get("cmds_error") else ""
            return _lane(lane_id, UNKNOWN, f"讀唔到板上指令表（status cmds）{extra}")
        if caps.get(cap_key):
            return _lane(lane_id, RUN, command=command, gate=gate)
        kind = facts.get("image_kind") or "unknown"
        return _lane(lane_id, BLOCKED,
                     f"板上 image 冇 {cap_pretty}（探到 image 類型：{kind}）；要用對應建置")

    port = board.get("port") or "<PORT>"
    lanes.append(image_lane("tests", "test_action", "0xF061（NC_ENABLE_TESTS）",
                            f"python3 tools/nc4.py --port {port} raw 0xF061 00",
                            "0xF061 回 producer／render JSON（唔係 err）"))
    lanes.append(image_lane("effect-perf", "effect_bench", "0xF065（EffectPerf）",
                            f"python3 tools/effect_bench.py --port {port}",
                            "每組 px 都有數字；同一 image 再跑一次對照"))

    if "ncm" in disabled:
        lanes.append(_lane("ncm", DISABLED, "machine.json：lane 已停用"))
    elif not _board_ready(facts):
        st = UNKNOWN if board.get("state") == "unknown" else BLOCKED
        lanes.append(_lane("ncm", st, _board_absent_reason(facts)))
    elif board.get("ncm") is None:
        lanes.append(_lane("ncm", UNKNOWN, "status ncm 冇回覆／解析不到"))
    elif board.get("ncm"):
        lanes.append(_lane("ncm", RUN,
                           command=f"python3 tools/nc4.py --port {port} status ncm",
                           gate="backend=wamr、ok:1、frames 增長"))
    else:
        lanes.append(_lane("ncm", BLOCKED, "板上 image 冇 ncm 段（release／非 NCM 建置）"))

    lanes.sort(key=lambda x: LANE_ORDER.index(x["id"]))
    summary = {k: sum(1 for x in lanes if x["status"] == k)
               for k in (RUN, BLOCKED, UNKNOWN, DISABLED)}

    return {
        "schema": 1,
        "generated_at": (facts.get("probed_at") or ""),
        "env": {
            "id": (machine.get("machine") or {}).get("id"),
            "label": (machine.get("machine") or {}).get("label"),
            "state_dir": (facts.get("state") or {}).get("shown"),
            "machine_file": "machine.json",
        },
        "facts": facts,
        "image_kind": facts.get("image_kind") or "unknown",
        "summary": summary,
        "lanes": lanes,
        "warnings": warnings,
        "drift": drift,
        "next": _next_steps(lanes, facts, machine, warnings),
    }


# ───────────────────────── 文字輸出 ─────────────────────────

def _mark(status: str) -> str:
    return STATUS_MARK.get(status, "?")


def _stamp_bits(facts: dict) -> dict:
    b = facts.get("board") or {}
    t = facts.get("target") or {}
    idf = (facts.get("idf") or {}).get("chosen") or {}
    repo = facts.get("repo") or {}
    board_name = t.get("board") or b.get("chip") or "-"
    label = str((facts.get("machine") or {}).get("label") or "-")
    return {
        "env": re.sub(r"\s+", "_", label),
        "idf": idf.get("version") or "-",
        "dev_idf": (b.get("sys") or {}).get("idf") or "-",
        "board": board_name if b.get("state") == "ready" else "-",
        "cid": b.get("cid_hex") or "-",
        "fw": (b.get("sys") or {}).get("fw") or "-",
        "app": (b.get("sys") or {}).get("app") or "-",
        "img": facts.get("image_kind") or "unknown",
        "git": repo.get("rev") or "-",
    }


def render_stamp(facts: dict) -> str:
    s = _stamp_bits(facts)
    order = ("env", "idf", "dev_idf", "board", "cid", "fw", "app", "img", "git")
    return " ".join(f"{k}={s[k]}" for k in order)


def stamp_dict(facts: dict) -> dict:
    return _stamp_bits(facts)


def _age_seconds(facts: dict) -> float | None:
    raw = facts.get("probed_at")
    if not raw:
        return None
    try:
        then = datetime.fromisoformat(raw)
        return max(0.0, (datetime.now().astimezone() - then).total_seconds())
    except ValueError:
        return None


def _age_text(facts: dict) -> str:
    raw = facts.get("probed_at")
    if not raw:
        return "時間未知"
    age = _age_seconds(facts)
    return f"{human_age(age)}前" if age is not None else raw


def _idf_line(facts: dict) -> str:
    idf = facts.get("idf") or {}
    chosen = idf.get("chosen") or {}
    if not chosen:
        return "找不到 ESP-IDF"
    if not chosen.get("valid"):
        return f"✘ {_short(chosen.get('path'))}（{idf.get('error') or '不是 IDF'}）"
    ver = chosen.get("version") or "版本未知"
    line = f"{ver} @ {_short(chosen.get('path'))}"
    if idf.get("matches") is False:
        line += f"  ⚠ 政策要求 {idf.get('required')}"
    elif idf.get("matches") is None and idf.get("required"):
        line += f"  ? 政策要求 {idf.get('required')}（讀唔到版本）"
    return line


def _board_line(facts: dict) -> str:
    b = facts.get("board") or {}
    t = facts.get("target") or {}
    if b.get("state") == "ready":
        sysd = b.get("sys") or {}
        name = t.get("board") or f"chip={b.get('chip')}"
        return (f"{name} · cid={b.get('cid_hex') or '?'} · fw={sysd.get('fw') or '?'} · "
                f"idf={sysd.get('idf') or '?'} · img={facts.get('image_kind')}")
    if b.get("state") == "unknown":
        return "未知：" + ("；".join(b.get("errors") or []) or "有 port 但冇 NC4 回覆")
    exp = t.get("board") or "未設定"
    extra = ""
    if t.get("expected_cid"):
        extra = f"，紀錄 cid={t['expected_cid']}"
    return f"未接（expected {exp}{extra}）"


def _build_line(repo: Path, facts: dict) -> str:
    art_facts = facts.get("artifacts") or {}
    art = art_facts.get("chosen")
    prefix = ""
    if not art:
        latest = art_facts.get("latest") or []
        art = latest[0] if latest else None
        prefix = "最新（未對應 expected）："
    if not art:
        return "—（artifacts 冇 image）"
    bits = []
    if art.get("tests") is not None:
        bits.append(f"tests={bool(art['tests'])}")
    if art.get("idf"):
        bits.append(f"idf={art['idf']}")
    if art.get("built_at"):
        bits.append(str(art["built_at"]).replace("T", " "))
    return prefix + f"{_rel(repo, art.get('bin'))}（{'，'.join(bits)}）"


def render_facts(facts: dict) -> str:
    """`tenv detect` 用的短摘要（唔列 lane）。"""
    repo = Path((facts.get("repo") or {}).get("root") or ".")
    board = facts.get("board") or {}
    ports = facts.get("ports") or []
    lines = [
        f"探測      {facts.get('probed_at')}（{_age_text(facts)}）",
        f"IDF       {_idf_line(facts)}",
        f"序列埠    {', '.join(ports) if ports else '—'}",
        f"板        {_board_line(facts)}",
    ]
    if board.get("state") == "ready":
        caps = board.get("caps") or {}
        cap_bits = []
        for key, label in (("test_action", "0xF061"), ("effect_bench", "0xF065"),
                           ("pixel_producer", "0xF060")):
            if key in caps:
                cap_bits.append(f"{label} {'✔' if caps[key] else '✘'}")
        lines.append(f"板上能力  {facts.get('image_kind')}"
                     + (f"（{'、'.join(cap_bits)}）" if cap_bits else "（指令表讀不到）"))
    lines.append(f"Build     {_build_line(repo, facts)}")
    if facts.get("errors"):
        lines.append(f"探測錯誤  {'；'.join(facts['errors'])}")
    return "\n".join(lines)


def render_text(plan: dict, verbose: bool = False) -> str:
    facts = plan["facts"]
    repo = Path((facts.get("repo") or {}).get("root") or ".")
    host = facts.get("host") or {}
    st = facts.get("state") or {}
    lines: list[str] = []
    lines.append(f"🩺 測試環境 — {plan['env'].get('label')}（{plan['env'].get('id')}）")
    lines.append("─" * 78)
    lines.append(f"狀態目錄  {st.get('shown')}（唔進 git）")
    lines.append(f"探測      {facts.get('probed_at')}（{_age_text(facts)}）")
    if host:
        tools = host.get("tools") or {}
        bits = [f"{host.get('os')} {host.get('release')} {host.get('arch')}",
                f"Python {host.get('python')}"]
        bits.append("bash " + ("✔" if tools.get("bash") else "✘"))
        bits.append("cc " + ("✔" if tools.get("cc") else "✘"))
        bits.append("pyserial " + ("✔" if tools.get("pyserial") else "✘"))
        lines.append("主機      " + " · ".join(bits))
    repo_f = facts.get("repo") or {}
    lines.append(f"Repo      {repo_f.get('rev') or '?'}（{'有未提交變更' if repo_f.get('dirty') else '乾淨'}）")
    lines.append(f"IDF       {_idf_line(facts)}")
    lines.append(f"板        {_board_line(facts)}")
    lines.append(f"Build     {_build_line(repo, facts)}")
    lines.append("")
    lines.append("LANE")
    for lane in plan["lanes"]:
        mark = _mark(lane["status"])
        head = f"{mark} {lane['id']:<12} {lane['title']}"
        lines.append(head)
        if lane["status"] == RUN:
            lines.append(f"   → {lane['command']}")
            if lane.get("gate"):
                lines.append(f"   閘門：{lane['gate']}")
        else:
            lines.append(f"   → {STATUS_LABEL[lane['status']]}：{lane['reason']}")
        for w in lane.get("warns") or []:
            lines.append(f"   ⚠ {w}")
        if verbose:
            lines.append(f"   需要：{'、'.join(lane['requires'])}　文件：{lane['doc']}")
    if plan["warnings"]:
        lines.append("")
        lines.append("注意")
        sym = {"warn": "⚠", "info": "·", "error": "✘"}
        for w in plan["warnings"]:
            lines.append(f"{sym.get(w['level'], '·')} {w['text']}")
    if plan.get("drift"):
        lines.append("")
        lines.append("同上次 probe 的分別")
        for d in plan["drift"]:
            lines.append(f"↻ {d['text']}")
    if plan.get("next"):
        lines.append("")
        lines.append("下一步")
        for n in plan["next"]:
            lines.append(f"• {n}")
    s = plan["summary"]
    lines.append("")
    lines.append(f"—— {s[RUN]} 可跑／{s[BLOCKED]} 暫停／{s[UNKNOWN]} 未知／{s[DISABLED]} 停用")
    lines.append("blocked／unknown 唔等於測過；回報數字前先貼 `python3 tools/tenv.py stamp`。")
    return "\n".join(lines)


def render_brief(plan: dict) -> str:
    facts = plan["facts"]
    repo = Path((facts.get("repo") or {}).get("root") or ".")
    st = facts.get("state") or {}
    repo_f = facts.get("repo") or {}
    lines = [
        "# 測試環境 brief",
        "",
        f"- env：`{plan['env'].get('label')}`（`{plan['env'].get('id')}`）",
        f"- 探測：{facts.get('probed_at')}（{_age_text(facts)}）· state：`{st.get('shown')}`",
        f"- repo：`{repo_f.get('rev') or '?'}`（{'dirty' if repo_f.get('dirty') else 'clean'}）",
        f"- IDF：{_idf_line(facts)}",
        f"- 板：{_board_line(facts)}",
        f"- build：`{_build_line(repo, facts)}`",
        "",
        "## Lanes",
        "| lane | 狀態 | 指令／原因 |",
        "|---|---|---|",
    ]
    for lane in plan["lanes"]:
        if lane["status"] == RUN:
            detail = f"`{lane['command']}`"
        else:
            detail = lane["reason"]
        detail = detail.replace("|", "\\|")
        lines.append(f"| {lane['id']} | {_mark(lane['status'])} {STATUS_LABEL[lane['status']]} | {detail} |")
    if plan["warnings"]:
        lines.append("")
        lines.append("## 注意")
        for w in plan["warnings"]:
            lines.append(f"- {w['text']}")
    if plan["next"]:
        lines.append("")
        lines.append("## 下一步")
        for n in plan["next"]:
            lines.append(f"- {n}")
    s = plan["summary"]
    lines += [
        "",
        "## 規矩",
        f"- {s[RUN]} 可跑／{s[BLOCKED]} 暫停／{s[UNKNOWN]} 未知／{s[DISABLED]} 停用；"
        "`blocked`／`unknown` 唔等於測過。",
        "- 回報數字前貼：`python3 tools/tenv.py stamp`；跨 IDF 版本數字唔可以互相對比。",
        "- 完整協議：`docs/02_guides/test-environment.md`。",
    ]
    return "\n".join(lines)
