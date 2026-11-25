"""現場探測：主機、repo、IDF、序列埠、板、板上能力、artifacts。

探測結果只講「看到甚麼」，唔講「應該點」。判斷留給 plan.py；這樣 selftest 可以用
假事實跑盡所有分支，不需要板。

為甚麼板上能力要靠 `status cmds` 而唔靠 artifact 檔名：
  · artifacts/ 唔進 git，同一塊板可能在其他電腦 build／OTA；本機 artifacts 經常
    對唔上板上 image。
  · 指令表是 image 自己講的：有 0xF061 = tests 映像、有 0xF065 = EffectPerf。
    這是 capability，不是猜檔案。
"""
from __future__ import annotations

import json
import os
import platform
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

from . import version_matches
from . import state

TOOLS = Path(__file__).resolve().parents[1]
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

from ncbuild import env as ncenv                      # noqa: E402
from nc4_demux import Reader, decode_str_u16, pack, str_u16   # noqa: E402

CMD_STATUS_GET = 0x1101
CMD_STATUS_RSP = 0x1102

# 板上四個 pixel 任務；右邊是舊 image 可能出現的 alias（見 AGENTS §5）。
PIXEL_ROLES = {
    "effect": ("pixel_effect_task", "pixel", "effect"),
    "paint": ("pixel_paint_task", "pair", "content", "show"),
    "encode": ("pixel_encode_task", "pixel_wave_task", "wave"),
    "render": ("render_task", "render"),
}


# ───────────────────────── 小工具 ─────────────────────────

def _run(args: list[str], cwd: Path | None = None, timeout: float = 8.0) -> tuple[int, str]:
    try:
        r = subprocess.run(args, cwd=str(cwd) if cwd else None, capture_output=True,
                           text=True, encoding="utf-8", errors="replace", timeout=timeout)
        return r.returncode, ((r.stdout or "") + (r.stderr or "")).strip()
    except (OSError, subprocess.TimeoutExpired):
        return 1, ""


def _os_name() -> str:
    name = platform.system().lower()
    return {"darwin": "macos", "windows": "windows"}.get(name, name or "unknown")


def _is_wsl() -> bool:
    if os.environ.get("WSL_DISTRO_NAME"):
        return True
    try:
        return "microsoft" in Path("/proc/version").read_text(encoding="utf-8", errors="ignore").lower()
    except OSError:
        return False


# ───────────────────────── 主機／repo ─────────────────────────

def host_facts() -> dict:
    try:
        import serial
        pyserial = getattr(serial, "Version", None) or getattr(serial, "__version__", "?")
    except Exception:
        pyserial = None
    os_name = _os_name()
    # macOS 的 platform.release() 是 Darwin kernel（例 25.x），不是 sw_vers 的
    # 產品版本；Windows 的 release() 又只回 "10"。報告要對得上用戶看到的版本。
    if os_name == "macos":
        release = platform.mac_ver()[0] or platform.release()
    elif os_name == "windows":
        release = platform.version()
    else:
        release = platform.release()
    return {
        "os": os_name,
        "release": release,
        "arch": platform.machine() or "unknown",
        "hostname": platform.node() or "unknown",
        "python": platform.python_version(),
        "cpu_count": os.cpu_count(),
        "wsl": _is_wsl(),
        "tools": {
            "bash": shutil.which("bash"),
            "cc": shutil.which("cc") or shutil.which("gcc") or shutil.which("clang"),
            "git": shutil.which("git"),
            "pyserial": pyserial,
            "wat2wasm": shutil.which("wat2wasm"),
        },
    }


def repo_facts(repo: Path) -> dict:
    rc, rev = _run(["git", "-C", str(repo), "rev-parse", "--short", "HEAD"])
    rc2, desc = _run(["git", "-C", str(repo), "describe", "--tags", "--always", "--dirty"])
    rc3, branch = _run(["git", "-C", str(repo), "rev-parse", "--abbrev-ref", "HEAD"])
    rc4, status = _run(["git", "-C", str(repo), "status", "--porcelain"])
    return {
        "root": str(repo),
        "rev": rev if rc == 0 and rev else None,
        "describe": desc if rc2 == 0 and desc else None,
        "branch": branch if rc3 == 0 and branch else None,
        "dirty": bool(rc4 == 0 and status),
    }


def board_meta(name: str | None) -> dict:
    """用 ports/ 的真相驗一塊板名，順便拿 chip 與 project 清單。"""
    if not name:
        return {"board": None, "valid": False, "error": "未設定 machine.board.expected"}
    try:
        import board as boardmod
        pairs = boardmod.find_ports(name)
        hw = pairs[0][0]
        meta = boardmod.load_board(hw)
        projects = boardmod.projects_for_hardware(hw)
        default = boardmod.default_project_for_hardware(hw)
        return {
            "board": meta.get("name", hw.name),
            "valid": True,
            "chip": meta.get("chip"),
            "projects": [p.name for p in projects],
            "default_project": default.name if default else None,
        }
    except SystemExit as e:
        return {"board": name, "valid": False, "error": str(e)}
    except Exception as e:                      # ports/ 壞掉也要有話說
        return {"board": name, "valid": False, "error": f"{type(e).__name__}: {e}"}


def target_facts(machine: dict) -> dict:
    """這台機「想」用哪塊板／project（machine.json 的意圖），對照 ports/ 驗一次。"""
    b = machine.get("board") or {}
    meta = board_meta(b.get("expected"))
    project = b.get("project")
    if meta.get("valid"):
        projects = meta.get("projects") or []
        if project and project not in projects:
            meta["error"] = f"{meta['board']} 冇 project {project!r}；有：{', '.join(projects)}"
            meta["valid"] = False
        elif not project:
            meta["project"] = meta.get("default_project")
        else:
            meta["project"] = project
        if not meta.get("project"):
            meta["project"] = meta.get("default_project")
    meta["expected_cid"] = (b.get("expected_cid") or "").upper() or None
    meta["absent_ok"] = bool(b.get("absent_ok"))
    return meta


# ───────────────────────── IDF ─────────────────────────

def idf_facts(repo: Path, machine: dict, policy: dict, chip: str | None) -> dict:
    cfg = ncenv.load_config()
    rows = []
    for source, path, explicit in ncenv.idf_candidates(chip, cfg):
        valid = ncenv.looks_like_idf(path)
        rows.append({
            "source": source,
            "path": str(path),
            "explicit": bool(explicit),
            "valid": bool(valid),
            "version": ncenv.idf_version(path) if valid else None,
        })

    pin = (machine.get("idf") or {}).get("pin")
    error = None
    if pin:
        p = Path(str(pin)).expanduser()
        chosen = next((r for r in rows if Path(r["path"]) == p), None)
        if chosen is None:
            chosen = {"source": "machine.idf.pin", "path": str(p), "explicit": True,
                      "valid": ncenv.looks_like_idf(p), "version": None}
            if chosen["valid"]:
                chosen["version"] = ncenv.idf_version(p)
            rows.insert(0, chosen)
        if not chosen["valid"]:
            error = f"machine.idf.pin 指住的路徑不是 ESP-IDF：{p}"
    else:
        explicit = [r for r in rows if r["explicit"]]
        chosen = explicit[0] if explicit else next((r for r in rows if r["valid"]), None)
        if chosen is not None and not chosen["valid"]:
            error = f"明確指定的 IDF 不存在（{chosen['source']}）：{chosen['path']}"

    required = (policy.get("idf") or {}).get("required")
    mode = (policy.get("idf") or {}).get("compare", "exact")
    matches = None
    if chosen is not None and chosen.get("valid"):
        matches = version_matches(chosen.get("version"), required, mode)

    return {
        "chosen": chosen,
        "candidates": rows,
        "required": required,
        "compare": mode,
        "source": (policy.get("idf") or {}).get("source"),
        "matches": matches,
        "error": error,
    }


# ───────────────────────── 板 ─────────────────────────

def _status_query(ser, rd: "Reader", keys: str, timeout: float) -> dict | None:
    ser.write(pack(CMD_STATUS_GET, str_u16(keys)))
    f = rd.wait_for({CMD_STATUS_RSP}, timeout)
    if f is None:
        return None
    text = decode_str_u16(f[1])
    if not text:
        return None
    try:
        data = json.loads(text)
    except ValueError:
        return None
    return data if isinstance(data, dict) else None


def _pixel_summary(tasks: list[dict] | None) -> dict | None:
    if tasks is None:
        return None
    out = {}
    for role, aliases in PIXEL_ROLES.items():
        t = next((x for x in tasks if isinstance(x, dict) and x.get("name") in aliases), None)
        if t is None:
            out[role] = None
            continue
        loops = [x for x in (t.get("loops") or []) if isinstance(x, dict)]
        out[role] = {
            "name": t.get("name"),
            "level": t.get("level"),
            "started": t.get("started"),
            "running": any(bool(x.get("running")) for x in loops),
        }
    return out


def _probe_one_port(port: str, baud: int, timeout: float) -> dict:
    out: dict = {"port": port, "ready": False, "error": None}
    try:
        import serial
    except Exception:
        out["error"] = "pyserial 未安裝（pip3 install pyserial）"
        return out
    try:
        s = serial.Serial(port, baud, timeout=0.2, write_timeout=2)
    except Exception as e:
        out["error"] = f"打唔開：{e}"
        return out

    try:
        time.sleep(0.35)          # TinyUSB CDC 枚舉後要一點時間才收資料
        rd = Reader(s)
        sysj = _status_query(s, rd, "sys", timeout)
        sysd = sysj.get("sys") if isinstance(sysj, dict) else None
        if not isinstance(sysd, dict):
            if isinstance(sysj, dict) and sysj.get("err"):
                out["error"] = f"STATUS 回 err={sysj['err']}"
            else:
                out["error"] = "冇 STATUS_RSP（app 未起／唔係 NC4／port 被其他程式佔住）"
            return out

        cmdsj = _status_query(s, rd, "cmds", min(timeout, 2.0))
        tasksj = _status_query(s, rd, "tasks", min(timeout, 2.0))
        ncmj = _status_query(s, rd, "ncm", 1.2)

        caps = None
        cmds_error = None
        codes: list[int] = []
        if isinstance(cmdsj, dict) and isinstance(cmdsj.get("cmds"), list):
            for item in cmdsj["cmds"]:
                if isinstance(item, dict) and isinstance(item.get("c"), int):
                    codes.append(item["c"])
            if codes:
                caps = {
                    "n": len(codes),
                    "test_action": 0xF061 in codes,     # NC_ENABLE_TESTS
                    "effect_bench": 0xF065 in codes,    # EffectPerf
                    "pixel_producer": 0xF060 in codes,
                    "layout_bench": 0xF077 in codes,
                    "pxbench": 0xF062 in codes,
                    "pixdiag": 0xF079 in codes,
                }
        elif isinstance(cmdsj, dict) and cmdsj.get("err"):
            cmds_error = str(cmdsj.get("err"))

        tasks = None
        tasks_truncated = False
        if isinstance(tasksj, dict) and isinstance(tasksj.get("tasks"), list):
            raw = tasksj["tasks"]
            if raw and isinstance(raw[0], dict) and raw[0].get("truncated"):
                tasks_truncated = True
            else:
                tasks = raw

        ncm = None
        if isinstance(ncmj, dict):
            ncm = bool(ncmj.get("ncm"))

        cid = sysd.get("cid")
        out.update({
            "ready": True,
            "sys": sysd,
            "cid_hex": f"{cid:04X}" if isinstance(cid, int) else None,
            "caps": caps,
            "cmds_error": cmds_error,
            "tasks": tasks,
            "tasks_truncated": tasks_truncated,
            "pixel": _pixel_summary(tasks),
            "ncm": ncm,
        })
        return out
    except Exception as e:
        out["error"] = f"{type(e).__name__}: {e}"
        return out
    finally:
        try:
            s.close()
        except Exception:
            pass


def board_facts(ports: list[str], policy: dict) -> dict:
    if not ports:
        return {"state": "absent", "ports": [], "errors": []}
    pr = policy.get("probe") or {}
    baud = int(pr.get("baud", 115200))
    timeout = float(pr.get("board_timeout_s", 2.5))
    probes = [_probe_one_port(p, baud, timeout) for p in ports]
    ready = next((p for p in probes if p.get("ready")), None)
    if ready:
        ready["state"] = "ready"
        ready["errors"] = [f"{p['port']}: {p['error']}" for p in probes if p.get("error")]
        return ready
    return {
        "state": "unknown",
        "ports": probes,
        "errors": [f"{p['port']}: {p['error']}" for p in probes if p.get("error")],
    }


def image_kind(board: dict) -> str:
    caps = board.get("caps")
    if board.get("state") != "ready" or not isinstance(caps, dict):
        return "unknown"
    if caps.get("effect_bench") and caps.get("test_action"):
        return "tests+effect"
    if caps.get("effect_bench"):
        return "effect-perf"
    if caps.get("test_action"):
        return "tests"
    return "release"


# ───────────────────────── artifacts／host tests ─────────────────────────

def artifact_facts(repo: Path, target: dict, policy: dict) -> dict:
    adir = repo / "artifacts"
    limit = int((policy.get("probe") or {}).get("max_artifacts", 40))
    if not adir.is_dir():
        return {"dir": "artifacts", "count": 0, "latest": [], "chosen": None,
                "error": "artifacts/ 不存在（未 build 過？）"}

    files = sorted(adir.glob("*.json"), key=lambda p: p.stat().st_mtime, reverse=True)[:limit]
    rows = []
    for f in files:
        try:
            data = json.loads(f.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
        if not isinstance(data, dict):
            continue
        name = str(data.get("board") or "")
        board, project = (name.split("__", 1) + [None])[:2] if "__" in name else (name, None)
        binp = adir / str(data.get("file") or "")
        rows.append({
            "name": name,
            "board": board,
            "project": project,
            "tests": data.get("tests"),
            "idf": data.get("idf"),
            "git": data.get("git"),
            "built_at": data.get("built_at"),
            "sha256": data.get("sha256"),
            "file": f"artifacts/{f.name}",
            "bin": str(binp),
            "bin_exists": bool(binp.is_file()),
        })

    rows.sort(key=lambda r: str(r.get("built_at") or ""), reverse=True)
    chosen = None
    board, project = target.get("board"), target.get("project")
    if board:
        wanted = [f"{board}__{project}"] if project else []
        wanted.append(str(board))
        for want in wanted:
            chosen = next((r for r in rows if r["name"] == want and r["bin_exists"]), None)
            if chosen:
                break
    return {
        "dir": "artifacts",
        "count": len(rows),
        "latest": rows[:8],
        "chosen": chosen,
    }


def host_test_facts(repo: Path, policy: dict) -> dict:
    hp = policy.get("host_tests") or {}
    runner = str(hp.get("runner") or "tools/hosttest/run_all.sh")
    expected = hp.get("expected")
    path = repo / runner
    tests: list[str] = []
    error = None
    if not path.is_file():
        error = f"找不到 {runner}"
    else:
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
            m = re.search(r'^TESTS="([^"]*)"', text, re.M)
            if m:
                tests = m.group(1).split()
            else:
                error = f"{runner} 找不到 TESTS= 清單"
        except OSError as e:
            error = str(e)
    missing = [t for t in tests if not (repo / "tools" / "hosttest" / t / "run.sh").is_file()]
    return {
        "runner": runner,
        "tests": tests,
        "count": len(tests),
        "missing": missing,
        "expected": expected,
        "error": error,
    }


# ───────────────────────── 總合 ─────────────────────────

def collect(repo: Path, state_dir: Path, machine: dict, policy: dict) -> dict:
    """探測這台機＋（如果有）板，回一份可以存成 JSON 的事實。"""
    errors: list[str] = []
    facts: dict = {
        "schema": 1,
        "probed_at": state.now_iso(),
        "machine": {
            "id": (machine.get("machine") or {}).get("id"),
            "label": (machine.get("machine") or {}).get("label"),
        },
        "policy": {
            "idf_required": (policy.get("idf") or {}).get("required"),
            "idf_source": (policy.get("idf") or {}).get("source"),
        },
    }

    try:
        facts["host"] = host_facts()
    except Exception as e:
        facts["host"] = {}
        errors.append(f"host: {type(e).__name__}: {e}")

    try:
        facts["repo"] = repo_facts(repo)
    except Exception as e:
        facts["repo"] = {"root": str(repo)}
        errors.append(f"repo: {type(e).__name__}: {e}")

    try:
        facts["target"] = target_facts(machine)
    except Exception as e:
        facts["target"] = {"board": None, "valid": False, "error": str(e)}
        errors.append(f"target: {type(e).__name__}: {e}")

    try:
        facts["idf"] = idf_facts(repo, machine, policy, (facts.get("target") or {}).get("chip"))
    except Exception as e:
        facts["idf"] = {"chosen": None, "candidates": [], "error": str(e)}
        errors.append(f"idf: {type(e).__name__}: {e}")

    try:
        ports = ncenv.serial_ports()
        facts["ports"] = ports
    except Exception as e:
        facts["ports"] = []
        errors.append(f"ports: {type(e).__name__}: {e}")

    try:
        facts["board"] = board_facts(facts.get("ports") or [], policy)
    except Exception as e:
        facts["board"] = {"state": "unknown", "ports": [], "errors": [str(e)]}
        errors.append(f"board: {type(e).__name__}: {e}")

    try:
        facts["artifacts"] = artifact_facts(repo, facts.get("target") or {}, policy)
    except Exception as e:
        facts["artifacts"] = {"count": 0, "latest": [], "chosen": None, "error": str(e)}
        errors.append(f"artifacts: {type(e).__name__}: {e}")

    try:
        facts["host_tests"] = host_test_facts(repo, policy)
    except Exception as e:
        facts["host_tests"] = {"count": 0, "tests": [], "error": str(e)}
        errors.append(f"host_tests: {type(e).__name__}: {e}")

    try:
        rel = state_dir.relative_to(repo)
        shown = str(rel)
    except ValueError:
        shown = str(state_dir)
    facts["state"] = {
        "dir": str(state_dir),
        "shown": shown,
        "exists": state_dir.is_dir(),
        "sync_risk": state.sync_risk(repo),
        "gitignored": state.is_gitignored(repo, state_dir) if (repo / ".git").exists() else None,
    }
    facts["errors"] = errors
    facts["image_kind"] = image_kind(facts.get("board") or {})
    return facts
