"""多晶片目標（S／C／P 系列）與「開機自動補齊」的測試。

用戶要求：
1. 開機檢查環境，有問題就順便自動安裝；
2. 會裝主流型號（S 系列／C 系列／P 系列），不只 S3。

所以這裡鎖住：晶片名驗證、preset 展開、預設組合、auto_plan 的三種模式、
以及「失敗後 6 小時內唔會重試」。
"""
import json
import time
from pathlib import Path

import pytest

import env


# ── 晶片目標 ────────────────────────────────────────────

def test_default_targets_cover_s_c_p():
    d = env.DEFAULT_TARGETS
    assert "esp32s3" in d          # S 系列
    assert "esp32c3" in d and "esp32c6" in d   # C 系列
    assert "esp32p4" in d          # P 系列


def test_normalize_none_and_empty_use_defaults():
    want = ",".join(env.DEFAULT_TARGETS)
    assert env.normalize_targets(None) == want
    assert env.normalize_targets("") == want
    assert env.normalize_targets([]) == want


def test_normalize_single_and_multi():
    assert env.normalize_targets("esp32s3") == "esp32s3"
    assert env.normalize_targets("esp32s3,esp32p4") == "esp32s3,esp32p4"
    assert env.normalize_targets(["esp32c3", "esp32s3"]) == "esp32c3,esp32s3"


def test_normalize_dedupes_and_keeps_order():
    assert env.normalize_targets("esp32s3,esp32s3,esp32c3") == "esp32s3,esp32c3"


def test_normalize_accepts_preset_name():
    assert env.normalize_targets("scp_mainstream") == "esp32s3,esp32c3,esp32c6,esp32p4"
    assert env.normalize_targets("p_only") == "esp32p4"


def test_normalize_all_alone_only():
    assert env.normalize_targets("all") == "all"
    with pytest.raises(ValueError):
        env.normalize_targets("all,esp32s3")


def test_normalize_rejects_unknown_chip():
    with pytest.raises(ValueError) as ei:
        env.normalize_targets("esp32x9")
    assert "esp32x9" in str(ei.value)
    assert "esp32s3" in str(ei.value)          # 有列可用值


def test_targets_label_is_readable():
    assert "ESP32-S3" in env.targets_label("esp32s3,esp32p4")


def test_install_command_uses_all_targets(monkeypatch):
    monkeypatch.setattr(env, "_is_windows", lambda: False)
    cmd = env.install_command("idf", "esp32s3,esp32c3,esp32p4", Path("/x/esp-idf"))
    assert cmd[-1] == "esp32s3,esp32c3,esp32p4"
    assert cmd[1].endswith("install.sh")


# ── auto_plan（開機自動補齊政策）────────────────────────

@pytest.fixture()
def fake_state(workdir, monkeypatch):
    monkeypatch.setattr(env, "AUTO_MARKER", workdir / ".env-auto.json")
    return workdir


def _report(ready: dict, tools: list | None = None):
    return {"ready": ready, "tools": tools or []}


def test_off_mode_never_installs(fake_state):
    rep = _report({"run_server": False, "flash_esptool": False, "build_firmware": False})
    assert env.auto_plan("off", rep) == []


def test_safe_mode_fixes_deps_and_esptool_but_not_idf(fake_state):
    rep = _report(
        {"run_server": False, "flash_esptool": False, "build_firmware": False},
        [{"id": "esptool", "status": "missing"}])
    plan = [p["what"] for p in env.auto_plan("safe", rep)]
    assert "deps" in plan and "esptool" in plan
    assert "idf" not in plan, "safe 模式唔應該自動裝 1–2 GB 的 IDF"


def test_all_mode_also_installs_idf(fake_state):
    rep = _report(
        {"run_server": True, "flash_esptool": True, "build_firmware": False},
        [{"id": "esptool", "status": "ok"}])
    plan = [p["what"] for p in env.auto_plan("all", rep)]
    assert plan == ["idf"]


def test_nothing_to_do_when_all_ready(fake_state):
    rep = _report({"run_server": True, "flash_esptool": True, "build_firmware": True},
                  [{"id": "esptool", "status": "ok"}])
    assert env.auto_plan("all", rep) == []


def test_failed_item_backs_off_for_cooldown(fake_state):
    rep = _report({"run_server": False, "flash_esptool": True, "build_firmware": True})
    assert env.auto_plan("safe", rep)                    # 未冷卻 → 會試
    env.auto_mark("deps", False)                          # 記一次失敗
    assert env.auto_plan("safe", rep) == []               # 冷卻中 → 唔再試


def test_success_does_not_block_but_condition_is_false(fake_state):
    """成功之後，如果條件已經滿足就自然唔會再排。"""
    env.auto_mark("deps", True)
    rep = _report({"run_server": True, "flash_esptool": True, "build_firmware": True})
    assert env.auto_plan("safe", rep) == []


def test_auto_marker_roundtrip(fake_state):
    env.auto_mark("idf", False)
    st = env.auto_last()
    assert st["idf"]["ok"] is False
    assert time.time() - st["idf"]["at"] < 60


def test_auto_plan_survives_missing_marker_dir(workdir, monkeypatch):
    monkeypatch.setattr(env, "AUTO_MARKER", workdir / "no" / "such" / ".env-auto.json")
    rep = _report({"run_server": False, "flash_esptool": True, "build_firmware": True})
    assert [p["what"] for p in env.auto_plan("safe", rep)] == ["deps"]


# ── report 要帶 targets / auto 資訊（UI 靠它）──────────────

def test_report_exposes_targets_and_auto():
    r = env.report()
    t = r["targets"]
    assert set(t["default"]) == set(env.DEFAULT_TARGETS)
    assert "esp32p4" in t["known"]
    assert "scp_mainstream" in t["presets"]
    assert "last" in r["auto"]


# ── API：target 可以係陣列／preset ────────────────────────

def test_api_install_accepts_preset_and_list(workdir, monkeypatch):
    import server
    from fastapi.testclient import TestClient

    monkeypatch.setattr(env, "AUTO_MARKER", workdir / ".env-auto.json")
    started = []

    def fake_start(what, **kw):
        started.append((what, kw))
        raise ValueError("stop-here")          # 唔想真的開 thread

    monkeypatch.setattr(env.INSTALLER, "start", fake_start)
    app = server.create_app(sim_root=workdir / "fs")
    with TestClient(app) as c:
        # preset 名
        c.post("/api/env/install", json={"what": "idf", "target": "p_only",
                                         "confirm": True})
        # 陣列
        c.post("/api/env/install", json={"what": "idf",
                                         "target": ["esp32s3", "esp32p4"],
                                         "confirm": True})
    assert started[0][1]["target"] == "p_only"
    assert started[1][1]["target"] == "esp32s3,esp32p4"
