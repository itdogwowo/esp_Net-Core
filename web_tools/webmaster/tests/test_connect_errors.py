"""自動連線失敗的訊息 + `/api/info` 的 autoconnectError。

情境（實際踩過）：已經有一個 WebMaster 開住 COM20，第二個再用 `autoconnect: "auto"`
去開同一個埠 → 開埠失敗。之前只係靜靜 `connected:false`，用戶會以為板壞。
"""
import json

import pytest
from fastapi.testclient import TestClient

import server


def make_app(workdir, autoconnect):
    return server.create_app(sim_root=workdir / "fs", autoconnect=autoconnect)


def test_friendly_message_for_busy_port():
    msg = server._friendly_connect_error("COM20", PermissionError("Access is denied"))
    assert "COM20" in msg
    assert "佔用" in msg
    # 要提示實際可行的做法
    assert "關掉" in msg or "連線" in msg


def test_friendly_message_for_timeout():
    msg = server._friendly_connect_error("COM7", TimeoutError("open timeout"))
    assert "timeout" in msg.lower() or "逾時" in msg


def test_friendly_message_passthrough_for_unknown():
    msg = server._friendly_connect_error("COM9", ValueError("weird"))
    assert "COM9" in msg and "weird" in msg


def test_auto_connect_failure_sets_error_and_info_exposes_it(workdir, monkeypatch):
    """模擬「埠被佔用」：autoconnect 指向一個連不上的埠。"""
    app = make_app(workdir, "COM_NOT_EXIST_9X")
    with TestClient(app) as c:
        info = c.get("/api/info").json()
        assert info["connected"] is False
        assert info["autoconnectError"], "連唔到就要有原因，唔可以靜靜 connected:false"
        assert "COM_NOT_EXIST_9X" in info["autoconnectError"]


def test_no_error_when_not_autoconnecting(workdir):
    app = make_app(workdir, None)
    with TestClient(app) as c:
        assert c.get("/api/info").json()["autoconnectError"] is None


def test_no_error_when_autoconnect_succeeds(workdir):
    app = make_app(workdir, "sim")
    with TestClient(app) as c:
        info = c.get("/api/info").json()
        assert info["connected"] is True
        assert info["autoconnectError"] is None
