import hashlib

import pytest
from fastapi.testclient import TestClient

import nc4
from server import create_app


@pytest.fixture()
def client(workdir):
    app = create_app(sim_root=workdir / "fs")
    with TestClient(app) as c:
        r = c.post("/api/connect", json={"sim": True})
        assert r.status_code == 200, r.text
        yield c


def test_info(client):
    j = client.get("/api/info").json()
    assert j["connected"] is True
    assert j["sim"] is True


def test_status(client):
    r = client.get("/api/status?keys=sys,storage")
    assert r.status_code == 200, r.text
    data = r.json()["data"]
    assert data["sys"]["chip"] == "esp32-s3"
    assert data["storage"]["mode"] == "app"


def test_manifest_scan_and_upload_download(client):
    assert client.get("/api/manifest").status_code == 404
    assert client.post("/api/scan").status_code == 200
    m = client.get("/api/manifest").json()["data"]
    assert "/config/system.json" in m

    put = client.put("/api/file?path=/config/ui.json", content=b'{"x":1}')
    assert put.status_code == 200, put.text
    assert put.json()["size"] == 7
    assert put.json()["sha256"] == hashlib.sha256(b'{"x":1}').hexdigest()

    got = client.get("/api/file?path=/config/ui.json")
    assert got.status_code == 200
    assert got.content == b'{"x":1}'

    empty = client.put("/api/file?path=/config/empty.bin", content=b"")
    assert empty.status_code == 200, empty.text
    assert empty.json()["size"] == 0
    assert client.get("/api/file?path=/config/empty.bin").content == b""


def test_confirm_undo_delete(client):
    client.put("/api/file?path=/config/a.txt", content=b"old")
    r = client.put("/api/file?path=/config/a.txt", content=b"new").json()
    assert r["pending"] is False          # server 自動 confirm
    assert r["auto_confirmed"] is True
    assert client.get("/api/file?path=/config/a.txt").content == b"new"

    # confirm / delete 仍然可用（例如 legacy pending）
    j = client.post("/api/file/confirm", json={"path": "/config/a.txt"}).json()
    assert j["data"]["pending"] is False

    j = client.post("/api/file/delete", json={"path": "/config/a.txt"}).json()
    assert j["data"]["exists"] is False


def test_upload_auto_confirm_removes_bak(client, workdir):
    client.put("/api/file?path=/config/b.txt", content=b"v1")
    r = client.put("/api/file?path=/config/b.txt", content=b"v2").json()
    assert r["auto_confirmed"] is True
    assert r["pending"] is False
    assert not (workdir / "fs" / "config" / "b.txt.bak").exists()
    assert client.get("/api/file?path=/config/b.txt").content == b"v2"
    assert client.get("/api/file?path=/config/a.txt").status_code == 404


def test_firmware_ota(client, workdir):
    data = bytes(range(256)) * 20
    r = client.post("/api/firmware?apply=1", content=data)
    assert r.status_code == 200, r.text
    assert r.json()["data"]["size"] == len(data)
    assert (workdir / "fs" / "firmware.bin").read_bytes() == data


def test_firmware_chip_guard(client):
    data = bytes(range(256)) * 10
    r = client.post("/api/firmware?expect_chip=esp32p4", content=data)
    assert r.status_code == 409, r.text
    det = r.json()["detail"]
    assert det["kind"] == "chip_mismatch"
    assert det["board"] == "esp32-s3"

    r = client.post("/api/firmware?expect_chip=esp32s3", content=data)
    assert r.status_code == 200, r.text


def test_cmd(client):
    payload = nc4.str_u16("sys")
    r = client.post("/api/cmd", json={
        "cmd": "0x1101", "payload_hex": payload.hex(),
        "expect": ["0x1102"], "timeout": 3,
    })
    assert r.status_code == 200, r.text
    j = r.json()
    assert j["cmd"] == "0x1102"
    assert j["json"]["sys"]["chip"] == "esp32-s3"


def test_ws_log_streams_lines(client):
    state = client.app.state.esp
    with client.websocket_connect("/ws/log") as ws:
        hello = ws.receive_json()
        assert hello["type"] == "hello"
        assert hello["connected"] is True
        # 連上後可能先收到 backlog（sim 連線時已有 log）；吃掉它再測即時流。
        msg = ws.receive_json()
        if msg["type"] == "backlog":
            msg = None
        state.broadcast({"type": "line", "text": "I (1) sim: hello", "ts": 1.0})
        ev = ws.receive_json()
        assert ev["type"] == "line"
        assert "hello" in ev["text"]


def test_shared_log_tail_and_activity(client):
    # 先讓 sim 產生一些 device log
    client.get("/api/status?keys=sys")
    tail = client.get("/api/log/tail?n=50").json()["events"]
    assert isinstance(tail, list) and tail
    last_seq = tail[-1]["seq"]
    more = client.get(f"/api/log/tail?n=50&since={last_seq}").json()["events"]
    assert all(e["seq"] > last_seq for e in more)

    r = client.post("/api/reboot", headers={"X-Actor": "ai"})
    assert r.status_code == 200
    acts = client.get("/api/activity?n=20").json()["events"]
    assert any(e.get("actor") == "ai" and "reboot" in e.get("action", "")
               for e in acts)

    client.post("/api/log/note", json={"actor": "ai", "text": "hello from agent"})
    acts = client.get("/api/activity?n=20").json()["events"]
    assert any(e.get("type") == "activity" and e.get("text") == "hello from agent"
               for e in acts)

    # AI 的讀取也要出現在活動（X-Actor: ai）
    client.get("/api/status?keys=sys", headers={"X-Actor": "ai"})
    acts = client.get("/api/activity?n=20").json()["events"]
    assert any(e.get("actor") == "ai" and e.get("action") == "GET /api/status"
               for e in acts)


def test_not_connected_returns_409(workdir):
    app = create_app(sim_root=workdir / "fs2")
    with TestClient(app) as c:
        r = c.get("/api/status")
        assert r.status_code == 409


def test_drive_list_read_write_delete(workdir):
    root = workdir / "drive"
    (root / "config").mkdir(parents=True)
    (root / "config" / "a.json").write_text('{"a":1}', encoding="utf-8")
    app = create_app(sim_root=workdir / "fs3")
    with TestClient(app) as c:
        j = c.get("/api/drive/list", params={"root": str(root), "path": "/config"}).json()
        assert [e["n"] for e in j["data"]["entries"]] == ["a.json"]
        assert j["data"]["path"] == "/config"

        r = c.get("/api/drive/file", params={"root": str(root), "path": "/config/a.json"})
        assert r.content == b'{"a":1}'

        r = c.put("/api/drive/file",
                  params={"root": str(root), "path": "/config/sub/b.txt"},
                  content=b"hello")
        assert r.status_code == 200, r.text
        assert (root / "config" / "sub" / "b.txt").read_bytes() == b"hello"

        r = c.post("/api/drive/delete", json={"root": str(root), "path": "/config/sub"})
        assert r.status_code == 400          # 資料夾要 recursive
        r = c.post("/api/drive/delete",
                   json={"root": str(root), "path": "/config/sub", "recursive": 1})
        assert r.status_code == 200
        assert not (root / "config" / "sub").exists()


def test_drive_rejects_traversal(workdir):
    root = workdir / "drive2"
    root.mkdir()
    app = create_app(sim_root=workdir / "fs4")
    with TestClient(app) as c:
        r = c.get("/api/drive/list", params={"root": str(root), "path": "/../"})
        assert r.status_code == 400
        r = c.get("/api/drive/file", params={"root": str(root), "path": "/../../etc/passwd"})
        assert r.status_code == 400


def test_devices_and_select(client):
    j = client.get("/api/devices").json()
    assert j["active"] == "sim"
    assert [d["device"] for d in j["devices"]] == ["sim"]
    assert client.get("/api/log/tail?n=5&device=sim").status_code == 200
    assert client.get("/api/log/tail?n=5&device=nope").status_code == 404
    r = client.post("/api/select", json={"device": "nope"})
    assert r.status_code == 404

    d = client.post("/api/disconnect").json()
    assert d["ok"] is True
    assert d["active"] is None
    assert d["devices"] == []
    assert client.get("/api/info").json()["connected"] is False


def test_empty_file_and_file_changed_ws(client):
    client.put("/api/file?path=/config/empty2.bin", content=b"")
    assert client.get("/api/file?path=/config/empty2.bin").content == b""
    with client.websocket_connect("/ws/log") as ws:
        assert ws.receive_json()["type"] == "hello"
        msg = ws.receive_json()
        if msg["type"] == "backlog":
            msg = None
        client.put("/api/file?path=/config/changed.txt", content=b"x")
        for _ in range(20):
            ev = ws.receive_json()
            if ev.get("type") == "file_changed" and ev.get("path") == "/config/changed.txt":
                assert ev.get("device") == "sim"
                break
        else:
            assert False, "冇收到 file_changed"
