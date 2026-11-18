"""AI 發起頁面燒錄：請求佇列、結果回報、取消。

為什麼唔係「AI 直接燒」：Web Serial 的 `requestPort()` 必須由 user gesture 觸發
（瀏覽器安全模型）。所以正確模型是 **AI 下指令 → 人按一下 → 結果回 AI**。
呢組測試鎖住佇列與狀態機。
"""
import pytest
from fastapi.testclient import TestClient

from server import create_app

IMG = bytes(range(256)) * 4          # 1 KiB 假映像


@pytest.fixture()
def client(workdir, monkeypatch):
    # 暫存映像要寫入測試自己的資料夾，唔好污染 repo 的 .flashq/
    import server as srv
    monkeypatch.setattr(srv, "FLASH_DIR", workdir / "flashq")
    app = create_app(sim_root=workdir / "fs")
    with TestClient(app) as c:
        yield c


def make_request(c, **params):
    q = {"offset": "0x0", "note": "測試"}
    q.update(params)
    return c.post("/api/flash/request", content=IMG, params=q,
                  headers={"X-Actor": "ai"})


# ── 路徑說明 ────────────────────────────────────────────

def test_flash_paths_lists_three_and_marks_ai(client):
    j = client.get("/api/flash/paths").json()["data"]
    ids = [p["id"] for p in j["paths"]]
    assert ids == ["nc4_ota", "server_esptool", "browser"]
    for p in j["paths"]:
        assert "aiCanInvoke" in p and "needsPortHandover" in p
    # 頁面燒錄也對 AI 開放（但要人按一下）
    assert j["paths"][2]["aiCanInvoke"] is True
    assert j["humanGestureRequired"] == ["browser"]


# ── 建立請求 ────────────────────────────────────────────

def test_create_request_returns_pending(client):
    r = make_request(client)
    assert r.status_code == 200, r.text
    d = r.json()["data"]
    assert d["status"] == "pending"
    assert d["size"] == len(IMG)
    assert d["by"] == "ai"
    assert d["offset"] == 0


def test_request_rejects_empty(client):
    r = client.post("/api/flash/request", content=b"", params={"offset": "0x0"})
    assert r.status_code == 400


def test_request_rejects_bad_offset(client):
    r = client.post("/api/flash/request", content=IMG, params={"offset": "zzz"})
    assert r.status_code == 400


def test_request_rejects_negative_offset(client):
    r = client.post("/api/flash/request", content=IMG, params={"offset": "-4"})
    assert r.status_code == 400


def test_request_rejects_too_large(client):
    huge = b"\0" * (8 * 1024 * 1024 + 1)
    r = client.post("/api/flash/request", content=huge, params={"offset": "0x0"})
    assert r.status_code == 413


def test_only_one_pending_at_a_time(client):
    assert make_request(client).status_code == 200
    r2 = make_request(client)
    assert r2.status_code == 409
    assert "待處理" in str(r2.json().get("detail"))


# ── 映像回讀 ────────────────────────────────────────────

def test_image_roundtrip(client):
    d = make_request(client).json()["data"]
    r = client.get(f"/api/flash/request/{d['id']}/image")
    assert r.status_code == 200
    assert r.content == IMG
    assert r.headers["content-type"] == "application/octet-stream"


def test_image_unknown_id_is_404(client):
    make_request(client)
    assert client.get("/api/flash/request/nope/image").status_code == 404


# ── 狀態機：claim → result ──────────────────────────────

def test_claim_then_result_done(client):
    d = make_request(client).json()["data"]
    rid = d["id"]
    assert client.post(f"/api/flash/request/{rid}/claim").status_code == 200
    r = client.post(f"/api/flash/request/{rid}/result", json={"ok": True})
    assert r.status_code == 200
    body = r.json()["data"]
    assert body["status"] == "done"
    # 完成之後就冇 pending
    assert client.get("/api/flash/request").json()["data"] is None
    # 但歷史讀得到（AI 事後查結果）
    hist = client.get("/api/flash/request").json()["history"]
    assert any(h["id"] == rid and h["status"] == "done" for h in hist)


def test_result_failed_records_error(client):
    d = make_request(client).json()["data"]
    rid = d["id"]
    client.post(f"/api/flash/request/{rid}/claim")
    r = client.post(f"/api/flash/request/{rid}/result",
                    json={"ok": False, "error": "requestPort 被拒絕"})
    assert r.json()["data"]["status"] == "failed"
    assert "requestPort" in r.json()["data"]["error"]


def test_claim_twice_is_409(client):
    d = make_request(client).json()["data"]
    rid = d["id"]
    assert client.post(f"/api/flash/request/{rid}/claim").status_code == 200
    assert client.post(f"/api/flash/request/{rid}/claim").status_code == 409


def test_cancel_clears_and_cleans_image(client):
    d = make_request(client).json()["data"]
    rid = d["id"]
    assert client.post(f"/api/flash/request/{rid}/cancel").status_code == 200
    assert client.get("/api/flash/request").json()["data"] is None
    # 取消之後可以再開新請求
    assert make_request(client).status_code == 200


def test_unknown_request_id_is_404(client):
    assert client.post("/api/flash/request/nope/claim").status_code == 404
    assert client.post("/api/flash/request/nope/cancel").status_code == 404
    assert client.post("/api/flash/request/nope/result",
                       json={"ok": True}).status_code == 404
