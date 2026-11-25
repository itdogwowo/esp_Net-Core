"""tenv 的離線自測：用假事實跑盡 plan.py 的分支。

為甚麼要有：lane 判斷係「唔好靜靜當測過」的最後一道閘；但真板唔係成日喺度。
呢支測試不需要板、不需要 IDF、不需要 network，三種機器都可以跑：

    python3 tools/tenv.py selftest
"""
from __future__ import annotations

import copy
import traceback
from . import plan as planmod
from . import state

ROOT = state.repo_root()
NOW = state.now_iso()

POLICY = {
    "schema": 1,
    "idf": {"required": "v6.1.1", "compare": "exact", "source": "test"},
    "host_tests": {"runner": "tools/hosttest/run_all.sh", "expected": 21},
    "probe": {},
}

MACHINE = {
    "machine": {"id": "test-id", "label": "test-machine"},
    "board": {
        "expected": "ESP32-S3-Matrix",
        "expected_cid": "2B4C",
        "project": "ESP32-S3-Matrix-LED-Textkit",
        "absent_ok": False,
    },
    "idf": {"pin": None},
    "lanes": {"disabled": []},
}

TASKS_OK = [
    {"name": "pixel_effect_task", "level": 4, "started": 1, "loops": [{"running": 1}]},
    {"name": "pixel_paint_task", "level": 4, "started": 1, "loops": [{"running": 1}]},
    {"name": "pixel_encode_task", "level": 4, "started": 1, "loops": [{"running": 1}]},
    {"name": "render_task", "level": 4, "started": 1, "loops": [{"running": 1}]},
]

PIXEL_OK = {
    "effect": {"name": "pixel_effect_task", "level": 4, "started": 1, "running": True},
    "paint": {"name": "pixel_paint_task", "level": 4, "started": 1, "running": True},
    "encode": {"name": "pixel_encode_task", "level": 4, "started": 1, "running": True},
    "render": {"name": "render_task", "level": 4, "started": 1, "running": True},
}


def _facts() -> dict:
    return {
        "schema": 1,
        "probed_at": NOW,
        "machine": {"id": "test-id", "label": "test-machine"},
        "policy": {"idf_required": "v6.1.1"},
        "host": {
            "os": "macos", "release": "14", "arch": "arm64", "hostname": "h",
            "python": "3.10", "wsl": False,
            "tools": {"bash": "/bin/bash", "cc": "/usr/bin/cc", "git": "/usr/bin/git",
                      "pyserial": "3.5", "wat2wasm": None},
        },
        "repo": {"root": str(ROOT), "rev": "abc1234", "dirty": False,
                 "describe": "abc1234", "branch": "main"},
        "target": {
            "board": "ESP32-S3-Matrix", "valid": True, "chip": "esp32s3",
            "project": "ESP32-S3-Matrix-LED-Textkit", "default_project": "ESP32-S3-Matrix",
            "projects": ["ESP32-S3-Matrix", "ESP32-S3-Matrix-LED-Textkit"],
            "expected_cid": "2B4C", "absent_ok": False,
        },
        "idf": {
            "chosen": {"path": "/x/esp-idf", "version": "v6.0.3", "valid": True,
                       "source": "~/esp/esp-idf", "explicit": False},
            "candidates": [{"path": "/x/esp-idf", "version": "v6.0.3", "valid": True,
                            "explicit": False, "source": "~/esp/esp-idf"}],
            "required": "v6.1.1", "compare": "exact", "source": "test",
            "matches": False, "error": None,
        },
        "ports": ["/dev/cu.usbmodem1"],
        "board": {
            "state": "ready", "port": "/dev/cu.usbmodem1", "cid_hex": "2B4C",
            "chip": "esp32s3",
            "sys": {"fw": "abc1234", "app": "11223344", "idf": "v6.0.3",
                    "chip": "esp32s3", "rst": 3},
            "caps": {"n": 20, "test_action": True, "effect_bench": False,
                     "pixel_producer": True, "layout_bench": False,
                     "pxbench": False, "pixdiag": False},
            "cmds_error": None,
            "tasks": TASKS_OK, "tasks_truncated": False,
            "pixel": PIXEL_OK, "ncm": False, "errors": [],
        },
        "artifacts": {
            "dir": "artifacts", "count": 1,
            "latest": [{"name": "ESP32-S3-Matrix__ESP32-S3-Matrix-LED-Textkit",
                        "board": "ESP32-S3-Matrix", "project": "ESP32-S3-Matrix-LED-Textkit",
                        "tests": False, "idf": "v6.0.3", "git": "abc1234",
                        "built_at": "2026-11-26T00:00:00",
                        "file": "artifacts/x.json", "bin": str(ROOT / "artifacts" / "x.bin"),
                        "bin_exists": True}],
            "chosen": {"name": "ESP32-S3-Matrix__ESP32-S3-Matrix-LED-Textkit",
                       "board": "ESP32-S3-Matrix", "project": "ESP32-S3-Matrix-LED-Textkit",
                       "tests": False, "idf": "v6.0.3", "git": "abc1234",
                       "built_at": "2026-11-26T00:00:00",
                       "file": "artifacts/x.json", "bin": str(ROOT / "artifacts" / "x.bin"),
                       "bin_exists": True},
        },
        "host_tests": {"runner": "tools/hosttest/run_all.sh",
                       "tests": [f"t{i}" for i in range(21)], "count": 21,
                       "expected": 21, "missing": [], "error": None},
        "state": {"dir": str(ROOT / ".testenv"), "shown": ".testenv", "exists": True,
                  "sync_risk": None, "gitignored": True},
        "errors": [],
        "image_kind": "tests",
    }


def _plan(facts: dict, machine: dict | None = None, last: dict | None = None) -> dict:
    return planmod.build_plan(facts, copy.deepcopy(machine or MACHINE), POLICY, last)


def _lane(p: dict, lane_id: str) -> dict:
    return next(x for x in p["lanes"] if x["id"] == lane_id)


def _has_warn(p: dict, code: str) -> bool:
    return any(w["code"] == code for w in p["warnings"])


def _eq(actual, want, label: str) -> None:
    assert actual == want, f"{label}: 期望 {want!r}，實際 {actual!r}"


def _contains(text: str, needle: str, label: str) -> None:
    assert needle in text, f"{label}: 找不到 {needle!r}；實際：{text!r}"


def case_host_only() -> None:
    f = _facts()
    f["ports"] = []
    f["board"] = {"state": "absent", "ports": [], "errors": []}
    f["image_kind"] = "unknown"
    f["target"]["absent_ok"] = True
    f["target"]["expected_cid"] = None
    p = _plan(f)
    _eq(_lane(p, "host")["status"], planmod.RUN, "host")
    _eq(_lane(p, "build")["status"], planmod.RUN, "build")
    _eq(_lane(p, "board")["status"], planmod.DISABLED, "board (absent_ok)")
    _eq(_lane(p, "ota")["status"], planmod.BLOCKED, "ota (no board)")
    _eq(_lane(p, "tests")["status"], planmod.BLOCKED, "tests (no board)")


def case_board_ready() -> None:
    p = _plan(_facts())
    for lid in ("host", "build", "board", "pixel", "ota", "tests"):
        _eq(_lane(p, lid)["status"], planmod.RUN, lid)
    _eq(_lane(p, "effect-perf")["status"], planmod.BLOCKED, "effect-perf")
    _eq(_lane(p, "ncm")["status"], planmod.BLOCKED, "ncm")
    assert _has_warn(p, "idf_policy"), "v6.0.3 vs 政策 v6.1.1 應該出 idf_policy warning"
    _eq(p["summary"]["runnable"], 6, "可跑 lane 數")
    _contains(planmod.render_text(p), "6 可跑", "文字摘要")


def case_port_unknown() -> None:
    f = _facts()
    f["board"] = {"state": "unknown", "ports": [],
                  "errors": ["/dev/cu.usbmodem1: 冇 STATUS_RSP"]}
    f["image_kind"] = "unknown"
    p = _plan(f)
    _eq(_lane(p, "board")["status"], planmod.UNKNOWN, "board (port 冇回覆)")
    _eq(_lane(p, "pixel")["status"], planmod.UNKNOWN, "pixel (跟 board)")
    _eq(_lane(p, "build")["status"], planmod.RUN, "build 不受板影響")


def case_pixel_broken() -> None:
    f = _facts()
    f["board"]["pixel"] = dict(f["board"]["pixel"])
    f["board"]["pixel"]["render"] = None
    f["board"]["tasks"] = [t for t in TASKS_OK if t["name"] != "render_task"]
    p = _plan(f)
    _eq(_lane(p, "pixel")["status"], planmod.BLOCKED, "pixel")
    _contains(_lane(p, "pixel")["reason"], "render", "pixel reason")


def case_cid_mismatch() -> None:
    f = _facts()
    f["board"]["cid_hex"] = "1111"
    p = _plan(f)
    assert _has_warn(p, "board_cid"), "cid 唔同應該出 warning"
    _eq(_lane(p, "board")["status"], planmod.RUN, "board 仍然可跑（只警告）")


def case_no_idf() -> None:
    f = _facts()
    f["idf"] = {"chosen": {"path": "/nope", "version": None, "valid": False,
                           "source": "env IDF_PATH", "explicit": True},
                "candidates": [], "required": "v6.1.1", "compare": "exact",
                "source": "test", "matches": None, "error": "IDF 不存在"}
    p = _plan(f)
    _eq(_lane(p, "build")["status"], planmod.BLOCKED, "build (no idf)")
    assert _has_warn(p, "idf_missing"), "應該出 idf_missing"
    _eq(_lane(p, "host")["status"], planmod.RUN, "host 不受 IDF 影響")


def case_tests_image_no() -> None:
    f = _facts()
    f["board"]["caps"] = dict(f["board"]["caps"], test_action=False)
    f["image_kind"] = "release"
    p = _plan(f)
    _eq(_lane(p, "tests")["status"], planmod.BLOCKED, "tests (release image)")
    _contains(_lane(p, "tests")["reason"], "0xF061", "tests reason")


def case_effect_image() -> None:
    f = _facts()
    f["board"]["caps"] = dict(f["board"]["caps"], test_action=False, effect_bench=True)
    f["image_kind"] = "effect-perf"
    p = _plan(f)
    _eq(_lane(p, "effect-perf")["status"], planmod.RUN, "effect-perf")
    _eq(_lane(p, "tests")["status"], planmod.BLOCKED, "tests (effect image)")


def case_drift() -> None:
    old = _facts()
    old["repo"]["rev"] = "deadbee"
    old["idf"]["chosen"]["version"] = "v6.1.1"
    old["board"]["sys"]["app"] = "aabbccdd"
    p = _plan(_facts(), last=old)
    codes = {d["code"] for d in p["drift"]}
    for want in ("repo_moved", "idf_changed", "device_reflashed"):
        assert want in codes, f"drift 少了 {want}：{codes}"


def case_versions() -> None:
    from . import version_matches
    assert version_matches("v6.1.1", "v6.1.1") is True, "同版"
    assert version_matches("v6.1", "v6.1.1") is False, "v6.1 ≠ v6.1.1"
    assert version_matches("v6.0.3", "v6.1.1") is False, "6.0.3 ≠ 6.1.1"
    assert version_matches(None, "v6.1.1") is None, "讀不到版本 = unknown，不是 False"


def case_machine_id_stable() -> None:
    host = {"os": "testos", "arch": "testarch"}
    a, b = state.make_machine_id(host), state.make_machine_id(host)
    _eq(a, b, "machine id 穩定")
    assert a.startswith("testos-testarch-"), f"machine id 格式不對：{a}"
    assert len(a.rsplit("-", 1)[-1]) == 12, f"machine id 指紋長度不對：{a}"


def case_disabled_lane() -> None:
    m = copy.deepcopy(MACHINE)
    m["lanes"]["disabled"] = ["ota"]
    p = _plan(_facts(), machine=m)
    _eq(_lane(p, "ota")["status"], planmod.DISABLED, "ota (machine 關掉)")


CASES = [
    ("host-only（absent_ok）", case_host_only),
    ("板齊、image=tests", case_board_ready),
    ("有 port 但冇 NC4 回覆", case_port_unknown),
    ("pixel 任務缺 render", case_pixel_broken),
    ("接錯板 cid", case_cid_mismatch),
    ("冇 IDF", case_no_idf),
    ("release image 冇 0xF061", case_tests_image_no),
    ("EffectPerf image", case_effect_image),
    ("紀錄漂移", case_drift),
    ("machine 關掉 lane", case_disabled_lane),
    ("IDF 版本比較", case_versions),
    ("machine id 穩定", case_machine_id_stable),
]


def run() -> int:
    print(f"tenv selftest — {len(CASES)} 個 case（唔需要板）")
    ok = 0
    for name, fn in CASES:
        try:
            fn()
        except AssertionError as e:
            print(f"✘ {name}\n  {e}")
        except Exception:
            print(f"✘ {name}\n{traceback.format_exc()}")
        else:
            ok += 1
            print(f"✔ {name}")
    print(f"—— {ok}/{len(CASES)}")
    return 0 if ok == len(CASES) else 1
