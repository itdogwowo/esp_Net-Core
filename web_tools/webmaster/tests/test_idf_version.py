"""ESP-IDF 版本解析與「版本化安裝」的測試。

為什麼要測：**`v6.1.0` 這個 tag 根本不存在**（ESP-IDF 用 `v6.1` + `v6.1.1`）。
如果寫死版本名，安裝會在 `git clone --branch` 就失敗，而錯誤訊息只會講
「Remote branch not found」——所以 normalize_version 要負責翻譯並講清楚。
"""
import pytest

import env

TAGS = ["v6.1.1", "v6.1", "v6.0.3", "v6.0", "v5.5.6", "v5.5", "v6.2-dev", "v6.1-rc1"]


# ── 版本清單整理 ────────────────────────────────────────

def test_stable_versions_filters_prerelease():
    st = env.stable_versions(TAGS)
    assert "v6.2-dev" not in st
    assert "v6.1-rc1" not in st
    assert st[0] == "v6.1.1"


# ── normalize_version ──────────────────────────────────

def test_empty_means_latest_stable():
    tag, note = env.normalize_version(None, TAGS)
    assert tag == "v6.1.1" and "最新" in note


def test_latest_keyword():
    tag, _ = env.normalize_version("latest", TAGS)
    assert tag == "v6.1.1"


def test_exact_tag():
    tag, note = env.normalize_version("v6.0.3", TAGS)
    assert tag == "v6.0.3" and note == ""


def test_adds_v_prefix():
    tag, note = env.normalize_version("6.1.1", TAGS)
    assert tag == "v6.1.1"
    assert "6.1.1" in note


def test_dot_zero_is_removed_because_tag_does_not_exist():
    """★ 核心：6.1.0 → v6.1（ESP-IDF 冇 .0 tag）。"""
    tag, note = env.normalize_version("6.1.0", TAGS)
    assert tag == "v6.1"
    assert "唔存在" in note and "v6.1" in note


def test_exact_series_tag_wins_over_patch():
    """`6.0` 而 `v6.0` 真係存在 → 用 v6.0（可預期，唔會偷偷升到修補版）。"""
    tag, note = env.normalize_version("6.0", TAGS)
    assert tag == "v6.0"
    assert note == "" or "v6.0" in note


def test_series_falls_back_to_latest_patch_when_series_tag_missing():
    """`6.4` 冇 `v6.4` 這個 tag，但有 `v6.4.2` → 用 v6.4.2。"""
    tags = ["v6.4.2", "v6.4.1", "v6.3.0x", "v6.3"]
    tag, note = env.normalize_version("6.4", tags)
    assert tag == "v6.4.2"
    assert "v6.4.2" in note


def test_unknown_version_warns_but_returns_something():
    tag, note = env.normalize_version("v9.9.9", TAGS)
    assert tag.startswith("v9.9.9")
    assert "警告" in note


def test_empty_tag_list_does_not_crash():
    tag, note = env.normalize_version("v6.1.1", [])
    assert tag == "v6.1.1"
    assert "離線" in note or "清單" in note


# ── clone 命令帶版本 ────────────────────────────────────

def test_clone_command_without_version():
    cmd = env.clone_command("C:/x", None)
    assert "--branch" not in cmd
    assert cmd[-1] == "C:\\x" or cmd[-1].endswith("x")


def test_clone_command_with_version():
    cmd = env.clone_command("C:/x", "v6.1.1")
    i = cmd.index("--branch")
    assert cmd[i + 1] == "v6.1.1"
    assert "--recursive" in cmd and "--depth" in cmd


# ── Installer 接版本 ───────────────────────────────────

def test_installer_start_accepts_version(monkeypatch):
    """用假的 Popen 避免真的 clone；只驗命令含 --branch 與版本。"""
    seen: list[list[str]] = []

    class FakeProc:
        def __init__(self, cmd, **kw):
            seen.append(cmd)
            self.stdout = iter(["fake line\n"])

        def wait(self):
            return 0

    monkeypatch.setattr(env.subprocess, "Popen", FakeProc)
    monkeypatch.setattr(env, "normalize_version", lambda v, t=None: ("v6.1.1", "用 v6.1.1"))
    inst = env.Installer()
    inst.start("idf", target="esp32s3", idf_dir=Path_fake(), confirm=True, version="6.1.0")
    inst._thread.join(timeout=20)
    assert seen, "應該有跑命令"
    first = seen[0]
    assert first[:2] == ["git", "clone"]
    assert "--branch" in first and "v6.1.1" in first


def Path_fake():
    import tempfile
    from pathlib import Path
    return Path(tempfile.mkdtemp()) / "esp-idf-nonexistent"
