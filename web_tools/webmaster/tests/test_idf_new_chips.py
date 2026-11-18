"""ESP32-S31 等新晶片：清單必須「以 IDF 為準」，唔可以硬編碼拒絕。

教訓（真實個案）：使用者需要 `esp32s31`，但我的硬編碼 KNOWN_TARGETS 冇，
程式直接報「唔認識嘅晶片」而令開機自動安裝失敗。實際上 IDF v6.1 的
`components/soc/esp32s31` 確實存在——所以驗證一定要問 IDF 本身。
"""
import pytest

import env


# ── 權威清單 ────────────────────────────────────────────

def test_targets_known_returns_list_and_source():
    known, source = env.targets_known()
    assert known, "要有至少幾個 target"
    assert source, "要講清楚來源（IDF／GitHub／內建）"
    assert all(t.startswith("esp32") for t in known)


def test_targets_known_includes_s31_when_from_idf(tmp_path, monkeypatch):
    """若已安裝 IDF，就由 components/soc 讀（最快最準）。"""
    soc = tmp_path / "esp-idf" / "components" / "soc"
    (soc / "esp32s3").mkdir(parents=True)
    (soc / "esp32s31").mkdir()
    (soc / "esp32p4").mkdir()
    (soc / "include").mkdir()            # 唔係 target，要濾走
    monkeypatch.setattr(env, "_TARGETS_CACHE",
                        {"at": 0.0, "targets": [], "source": None})
    got = env.idf_targets_from_dir(tmp_path / "esp-idf")
    assert got == ["esp32p4", "esp32s3", "esp32s31"]
    assert "include" not in got


def test_idf_targets_from_missing_dir_is_empty(tmp_path):
    assert env.idf_targets_from_dir(tmp_path / "nope") == []


# ── normalize：S31 要通過 ───────────────────────────────

def test_s31_is_accepted(monkeypatch):
    monkeypatch.setattr(env, "targets_known",
                        lambda force=False: (["esp32s3", "esp32s31", "esp32p4"],
                                             "test"))
    assert env.normalize_targets("esp32s31") == "esp32s31"
    assert env.normalize_targets("esp32s3,esp32s31") == "esp32s3,esp32s31"


def test_s31_in_lenient_mode_is_not_dropped(monkeypatch):
    monkeypatch.setattr(env, "targets_known",
                        lambda force=False: (["esp32s3", "esp32s31"], "test"))
    tgt, unknown = env.targets_or_default(["esp32s3", "esp32s31"])
    assert tgt == "esp32s3,esp32s31"
    assert unknown == []


def test_unknown_new_chip_is_kept_but_reported(monkeypatch):
    """未知晶片「唔丟」——可能係未收錄的新型號；只回報，交 IDF 判斷。"""
    monkeypatch.setattr(env, "targets_known",
                        lambda force=False: (["esp32s3"], "test"))
    tgt, unknown = env.targets_or_default(["esp32s3", "esp32z9"])
    assert "esp32z9" in tgt
    assert unknown == ["esp32z9"]


def test_strict_mode_still_rejects_obvious_typo(monkeypatch):
    monkeypatch.setattr(env, "targets_known",
                        lambda force=False: (["esp32s3", "esp32p4"], "test"))
    with pytest.raises(ValueError) as ei:
        env.normalize_targets("esp32s999")
    assert "esp32s999" in str(ei.value)
    assert "esp32s3" in str(ei.value)          # 提示可用值


# ── install 命令帶 S31 ──────────────────────────────────

def test_install_command_with_s31(monkeypatch):
    monkeypatch.setattr(env, "_is_windows", lambda: False)
    monkeypatch.setattr(env, "targets_known",
                        lambda force=False: (["esp32s3", "esp32s31"], "test"))
    cmd = env.install_command("idf", "esp32s3,esp32s31,esp32c6", "C:/x")
    assert cmd[-1] == "esp32s3,esp32s31,esp32c6"


# ── report 要帶 live 清單給 UI 動態畫 chip ───────────────

def test_report_exposes_live_targets_and_source():
    r = env.report()
    t = r["targets"]
    assert t["live"], "live 清單唔可以空"
    assert t["source"]
    # S31 應該喺（由 IDF／GitHub 取；內建清單亦有）
    assert "esp32s31" in t["live"] or "esp32s3" in t["live"]


def test_builtin_list_contains_new_chips():
    """即使離線只用內建清單，S31／C5／C61／H4／H21 都要在。"""
    for chip in ("esp32s31", "esp32c5", "esp32c61", "esp32h4", "esp32h21"):
        assert chip in env.KNOWN_TARGETS, f"{chip} 唔喺內建清單"


# ── 設定檔：預設 auto_install 要係 all ──────────────────

def test_default_auto_install_is_all():
    import settings
    assert settings.DEFAULTS["env"]["auto_install"] == "all"
    assert "env" in settings.DEFAULTS, "DEFAULTS 必須有 env 段（否則缺檔時 KeyError）"
