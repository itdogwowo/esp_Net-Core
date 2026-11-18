"""序列埠交接（頁面燒錄 esptool-js）的 API 測試。

COM 埠同一時間只有一個主人：要做瀏覽器燒錄，server 必須**主動放開**並記住
「交咗出去」，呢段時間唔可以偷偷重開；交還之後要重新連線。
呢幾條就是那個狀態機，壞了會直接導致「PermissionError」或「兩個主人搶埠」。
"""
import pytest
from fastapi.testclient import TestClient

from server import create_app


@pytest.fixture()
def client(workdir):
    app = create_app(sim_root=workdir / "fs")
    with TestClient(app) as c:
        assert c.post("/api/connect", json={"sim": True}).status_code == 200
        yield c


def test_state_initial(client):
    j = client.get("/api/port/state").json()
    assert j["handover"] is False
    assert j["heldByBrowser"] is False
    assert j["port"] == "sim"
    assert j["stale"] is False
    assert j["browserFlash"] is True


def test_release_then_state_says_handover(client):
    r = client.post("/api/port/release")
    assert r.status_code == 200, r.text
    assert r.json()["handover"] is True
    j = client.get("/api/port/state").json()
    assert j["handover"] is True
    assert j["port"] == "sim"
    assert j["ageSec"] is not None and j["ageSec"] >= 0


def test_require_blocks_while_handover(client):
    """交接期間，其他 API 要用「已經交畀瀏覽器」的訊息擋住，而唔係「未連線」。"""
    client.post("/api/port/release")
    r = client.get("/api/status?keys=sys")
    assert r.status_code == 409
    detail = str(r.json().get("detail"))
    assert "瀏覽器" in detail and "交還" in detail


def test_reclaim_reconnects(client):
    client.post("/api/port/release")
    r = client.post("/api/port/reclaim")
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["handover"] is False
    assert body["port"] == "sim"
    # 交還之後 status 又要能用
    assert client.get("/api/status?keys=sys").status_code in (200, 504)


def test_release_twice_is_409(client):
    client.post("/api/port/release")
    r = client.post("/api/port/release")
    assert r.status_code == 409, "已經交出去就唔應該再交一次"


def test_release_without_connection_is_409(workdir):
    app = create_app(sim_root=workdir / "fs2")     # 冇 autoconnect
    with TestClient(app) as c:
        assert c.post("/api/port/release").status_code == 409


def test_reclaim_without_handover_still_works_if_port_known(client):
    """冇交出去就交還：唔應該爆；用 query 指定埠就照連。"""
    r = client.post("/api/port/reclaim?port=sim")
    assert r.status_code == 200, r.text
    assert r.json()["handover"] is False


def test_settings_can_disable_browser_flash(workdir, monkeypatch):
    import settings

    app = create_app(sim_root=workdir / "fs3")
    real_load = settings.load

    def fake_load(base=None, *, quiet=False):
        conf, used = real_load(base, quiet=quiet)
        conf = dict(conf)
        conf["flash"] = dict(conf.get("flash") or {})
        conf["flash"]["browser_flash"] = False
        return conf, used

    monkeypatch.setattr(settings, "load", fake_load)
    with TestClient(app) as c:
        assert c.get("/api/port/state").json()["browserFlash"] is False
