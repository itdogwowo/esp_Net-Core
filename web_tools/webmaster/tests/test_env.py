"""環境自檢（`env.py`）與安裝 API 的測試。

重點：
1. 探測**只讀**、唔會 raise（缺工具就回 missing，唔會爆）；
2. 報告要分得清「燒錄」同「建置」兩種能力（用戶最常混淆）；
3. 安裝 API **一定要 confirm**，而且唔會使用非空目錄覆蓋（安全）；
4. 一次只准一個安裝工作。
"""
import json
from pathlib import Path

import pytest
from fastapi.testclient import TestClient

import env
import server


# ── 探測 ────────────────────────────────────────────────

def test_report_shape():
    r = env.report()
    assert "os" in r and "tools" in r and "ready" in r
    assert isinstance(r["tools"], list) and len(r["tools"]) >= 8
    assert set(r["ready"]) == {
        "run_server", "flash_nc4_ota", "flash_esptool",
        "flash_browser", "build_firmware"}
    for t in r["tools"]:
        assert t["id"] and t["status"] in ("ok", "warn", "missing")
        assert "label" in t and "impact" in t


def test_report_never_installs(monkeypatch):
    """探測會跑 `--version` 之類（正常），但**唔可以**跑安裝／clone。"""
    called = []

    def boom(*a, **kw):
        called.append(a)
        raise AssertionError("探測階段唔應該跑安裝命令")

    monkeypatch.setattr(env, "install_command", boom)
    monkeypatch.setattr(env, "clone_command", boom)
    env.report()
    assert not called


def test_build_ready_requires_idf(workdir, monkeypatch):
    """冇 IDF → build_firmware=False；但燒錄能力唔受影響。

    注意：`_check_idf()` 仲會 fallback 去 `default_idf_dir()`（例如這台機真的
    裝了 ~/esp/esp-idf）。測試要 hermetic，所以連佢都指去一個唔存在的位置，
    否則「有 IDF 的機器」會 fail、「冇 IDF 的 CI」pass。
    """
    no_idf = workdir / "no-idf"
    monkeypatch.setattr(env, "_idf_candidates", lambda: [])
    monkeypatch.setattr(env, "default_idf_dir", lambda: no_idf)
    monkeypatch.setattr(env, "_check_toolchain",
                        lambda: {"id": "idf_toolchain", "status": "missing"})
    r = env.report()
    assert r["ready"]["build_firmware"] is False
    assert r["ready"]["flash_nc4_ota"] is True      # 燒錄唔靠 IDF
    assert "idf" in r["missing"]


# ── 安裝命令組裝（唔執行）──────────────────────────────

def test_install_command_deps_uses_venv_python():
    cmd = env.install_command("deps")
    assert cmd[0].endswith(("python.exe", "python"))
    assert "-m" in cmd and "pip" in cmd
    assert any("requirements.txt" in c for c in cmd)


def test_install_command_esptool():
    """esptool 要「升到最新」而唔止「有裝就算」：要有 --upgrade。"""
    cmd = env.install_command("esptool")
    assert "--upgrade" in cmd
    assert "esptool" in cmd
    assert cmd[0].endswith(("python.exe", "python"))


def test_install_command_idf_windows(monkeypatch):
    monkeypatch.setattr(env, "_is_windows", lambda: True)
    cmd = env.install_command("idf", "esp32s3", Path("C:/x/esp-idf"))
    assert cmd[0] == "powershell"
    assert "install.ps1" in " ".join(cmd)
    assert cmd[-1] == "esp32s3"


def test_install_command_idf_posix(monkeypatch):
    monkeypatch.setattr(env, "_is_windows", lambda: False)
    cmd = env.install_command("idf", "esp32s3", Path("/x/esp-idf"))
    assert cmd[0] == "bash"
    assert cmd[1].endswith("install.sh")


def test_install_command_idf_rejects_bad_target(monkeypatch):
    monkeypatch.setattr(env, "_is_windows", lambda: False)
    inst = env.Installer()
    with pytest.raises(ValueError):
        inst.start("idf", target="esp32s3; rm -rf /", confirm=True)


def test_clone_command_is_official_repo():
    cmd = env.clone_command(Path("/tmp/x"))
    assert cmd[:2] == ["git", "clone"]
    assert "https://github.com/espressif/esp-idf.git" in cmd


def test_install_command_rejects_unknown():
    with pytest.raises(ValueError):
        env.install_command("rm-rf")


# ── Installer 狀態機 ───────────────────────────────────

def test_installer_requires_confirm():
    inst = env.Installer()
    with pytest.raises(ValueError):
        inst.start("deps", confirm=False)


def test_installer_rejects_unknown_what():
    inst = env.Installer()
    with pytest.raises(ValueError) as ei:
        inst.start("delete_everything", confirm=True)
    assert "唔認識" in str(ei.value)


def test_installer_refuses_non_empty_non_idf_dir(tmp_path):
    """非空又唔係 IDF repo → 停手（唔會覆蓋用戶資料）。"""
    d = tmp_path / "not-idf"
    d.mkdir()
    (d / "my-precious.txt").write_text("keep me", encoding="utf-8")
    inst = env.Installer()
    inst.start("idf", target="esp32s3", idf_dir=d, confirm=True)
    inst._thread.join(timeout=20)
    snap = inst.snapshot()
    assert snap["running"] is False
    assert snap["rc"] == -2
    assert (d / "my-precious.txt").exists(), "唔可以刪／覆蓋用戶檔案"
    assert any("停手" in line for line in snap["lines"])


def test_installer_snapshot_since(tmp_path):
    inst = env.Installer()
    inst.log("a\nb\nc")
    s1 = inst.snapshot(0)
    assert s1["total"] == 3 and s1["lines"] == ["a", "b", "c"]
    s2 = inst.snapshot(2)
    assert s2["lines"] == ["c"]


# ── API ────────────────────────────────────────────────

@pytest.fixture()
def client(workdir):
    app = server.create_app(sim_root=workdir / "fs")
    with TestClient(app) as c:
        yield c


def test_api_env(client):
    r = client.get("/api/env")
    assert r.status_code == 200
    d = r.json()["data"]
    assert "tools" in d and "ready" in d
    json.dumps(d)                       # 一定係可序列化


def test_api_install_requires_confirm(client):
    r = client.post("/api/env/install", json={"what": "deps"})
    assert r.status_code == 400
    assert "確認" in str(r.json().get("detail"))


def test_api_install_unknown_what(client):
    r = client.post("/api/env/install", json={"what": "nope", "confirm": True})
    assert r.status_code == 400


def test_api_install_log_endpoint(client):
    r = client.get("/api/env/install?since=0")
    assert r.status_code == 200
    d = r.json()["data"]
    assert "running" in d and "lines" in d and "total" in d
