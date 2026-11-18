"""`POST /api/env/config`：把 env.* 寫入 settings.local.json。

場景：IDF 裝喺邊係「這台機」的事（受限環境唔准寫家目錄），
用戶應該在 UI 一按就生效，唔需要手改 JSON；而且**唔應該**改到團隊的 settings.json。
"""
import json

import pytest
from fastapi.testclient import TestClient

import server
import settings


@pytest.fixture()
def local_dir(workdir, monkeypatch):
    """把 settings.json / settings.local.json 指向測試資料夾。"""
    d = workdir / "cfg"
    d.mkdir()
    monkeypatch.setattr(settings, "TOOL_ROOT", d)
    (d / "settings.json").write_text(json.dumps({"server": {"port": 8011}}),
                                     encoding="utf-8")
    return d


@pytest.fixture()
def client(workdir, local_dir):
    app = server.create_app(sim_root=workdir / "fs")
    with TestClient(app) as c:
        yield c


def test_write_idf_dir(client, local_dir):
    r = client.post("/api/env/config", json={"idf_dir": "D:/esp/esp-idf"})
    assert r.status_code == 200, r.text
    assert r.json()["file"] == "settings.local.json"
    data = json.loads((local_dir / "settings.local.json").read_text(encoding="utf-8"))
    assert data["env"]["idf_dir"] == "D:/esp/esp-idf"
    # 團隊的 settings.json 唔應該被改
    assert "idf_dir" not in (local_dir / "settings.json").read_text(encoding="utf-8")


def test_write_targets_and_version(client, local_dir):
    r = client.post("/api/env/config", json={
        "idf_targets": ["esp32s3", "esp32s31", "esp32p4"],
        "idf_version": "v6.1.1",
        "auto_install": "all",
    })
    assert r.status_code == 200, r.text
    env = r.json()["env"]
    assert env["idf_targets"] == ["esp32s3", "esp32s31", "esp32p4"]
    assert env["idf_version"] == "v6.1.1"
    assert env["auto_install"] == "all"


def test_merge_keeps_previous_values(client, local_dir):
    client.post("/api/env/config", json={"idf_dir": "D:/a"})
    client.post("/api/env/config", json={"idf_version": "v6.1"})
    data = json.loads((local_dir / "settings.local.json").read_text(encoding="utf-8"))
    assert data["env"]["idf_dir"] == "D:/a"        # 之前寫的仍然在
    assert data["env"]["idf_version"] == "v6.1"


def test_rejects_unknown_keys_only(client):
    r = client.post("/api/env/config", json={"nope": 1})
    assert r.status_code == 400
    assert "冇可寫欄位" in str(r.json().get("detail"))


def test_rejects_bad_value_via_schema(client):
    r = client.post("/api/env/config", json={"auto_install": "sometimes"})
    assert r.status_code == 400
    assert "auto_install" in str(r.json().get("detail"))


def test_reports_files_used(client):
    j = client.post("/api/env/config", json={"idf_dir": "D:/a"}).json()
    assert "settings.json" in j["files"]
    assert "settings.local.json" in j["files"]
