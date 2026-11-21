"""`/api/build`、`/api/build/info`、`/api/build/artifact` 的測試（**唔會真 build**）。

真的 IDF build 要好幾分鐘＋工具鏈，測試全部 mock `subprocess.run`，只驗：
1. command 組裝：`sys.executable`、cwd=repo 根、`--project/--tests/--fresh` 次序、
   **冇 shell=True**、timeout=600、env 帶 `IDF_PATH`（同 `PYTHONUTF8`）；
2. 輸出解析：`lines`／`artifact`（由 make.py 印的「產物 …」）／`size`／`elapsed`；
3. 失敗／timeout 都係 HTTP 200 + ok:false（唔會 500），artifact 留空；
4. `/api/build/info` 由 `ports/` 掃 boards/projects，唔跑 make.py；
5. artifact endpoint 只准 `repo/artifacts/*.bin`（path traversal／非 .bin 要拒絕）。
"""
import json
import subprocess
import sys
from pathlib import Path

import pytest
from fastapi.testclient import TestClient

import server

BOARD = "ESP32-S3-Matrix"
PROJECT = "ESP32-S3-Matrix-Show"


def _write(path: Path, data: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(data, encoding="utf-8")


@pytest.fixture()
def fake_repo(workdir, monkeypatch):
    """假 repo：make.py ＋ ports/hardware ＋ ports/project ＋ 一個假 IDF。"""
    root = workdir / "repo"
    _write(root / "make.py", "#!/usr/bin/env python3\nprint('fake make.py')\n")
    _write(root / "ports/hardware/S3/ESP32-S3-Matrix/board.json",
           json.dumps({"name": BOARD, "chip": "esp32s3"}))
    _write(root / "ports/hardware/P4/ESP32-P4-ETH/board.json",
           json.dumps({"name": "ESP32-P4-ETH", "chip": "esp32p4"}))
    _write(root / "ports/project/S3/ESP32-S3-Matrix/project.json",
           json.dumps({"name": BOARD, "hardware": "S3/ESP32-S3-Matrix",
                       "default": True, "seed": ["hardware"]}))
    _write(root / "ports/project/S3/ESP32-S3-Matrix-Show/project.json",
           json.dumps({"name": PROJECT, "hardware": "S3/ESP32-S3-Matrix"}))
    idf = workdir / "fake-idf"
    (idf / "tools").mkdir(parents=True)
    (idf / "tools" / "idf.py").write_text("# fake idf.py\n", encoding="utf-8")
    monkeypatch.setattr(server, "REPO_ROOT", root)
    monkeypatch.setattr(server, "_resolve_build_idf", lambda: (idf, "test"))
    return {"root": root, "idf": idf}


class FakeProc:
    def __init__(self, returncode: int = 0, stdout: str = ""):
        self.returncode = returncode
        self.stdout = stdout


@pytest.fixture()
def client(workdir, fake_repo):
    app = server.create_app(sim_root=workdir / "fs")
    with TestClient(app) as c:
        yield c


def _mock_run(monkeypatch, calls, *, returncode=0, stdout="", before=None):
    def fake_run(cmd, **kwargs):
        calls.append((cmd, kwargs))
        if before is not None:
            before()
        return FakeProc(returncode, stdout)
    monkeypatch.setattr(server.subprocess, "run", fake_run)


# ── info ───────────────────────────────────────────────

def test_build_info_lists_boards_and_projects(client, fake_repo):
    r = client.get("/api/build/info")
    assert r.status_code == 200, r.text
    data = r.json()["data"]
    assert data["repo"] == str(fake_repo["root"])
    assert data["makePyExists"] is True
    assert data["python"] == sys.executable
    assert data["timeout"] == 600
    assert data["idfPath"] == str(fake_repo["idf"])
    assert data["canBuild"] is True
    boards = {b["name"]: b for b in data["boards"]}
    assert set(boards) == {BOARD, "ESP32-P4-ETH"}
    matrix = boards[BOARD]
    assert matrix["chip"] == "esp32s3"
    names = [p["name"] for p in matrix["projects"]]
    assert names == [BOARD, PROJECT]          # default（同名）排最前
    assert matrix["projects"][0]["default"] is True
    assert PROJECT in data["projects"]
    assert data["lastBuild"] is None


def test_build_info_without_make_py(workdir, monkeypatch):
    empty = workdir / "empty-repo"
    empty.mkdir()
    monkeypatch.setattr(server, "REPO_ROOT", empty)
    monkeypatch.setattr(server, "_resolve_build_idf", lambda: (None, ""))
    app = server.create_app(sim_root=workdir / "fs2")
    with TestClient(app) as c:
        data = c.get("/api/build/info").json()["data"]
    assert data["makePyExists"] is False
    assert data["idfPath"] is None
    assert data["canBuild"] is False
    assert data["boards"] == [] and data["projects"] == []


# ── POST /api/build ────────────────────────────────────

def test_build_runs_make_py_without_shell(client, fake_repo, monkeypatch):
    calls = []

    def create_artifact():
        art = fake_repo["root"] / "artifacts" / f"{BOARD}.bin"
        art.parent.mkdir(parents=True, exist_ok=True)
        art.write_bytes(b"\x00" * 321)
        (art.with_suffix(".json")).write_text(
            json.dumps({"chip": "esp32s3"}), encoding="utf-8")

    stdout = (
        "→ 建置 ESP32-S3-Matrix（esp32s3，IDF v6.1）\n"
        "✔ 建置完成  ESP32-S3-Matrix  (12s)\n"
        "  image   511.3 KiB  (0x7fdc0)\n"
        f"  產物    artifacts/{BOARD}.bin，+12 B vs 上一顆\n"
    )
    _mock_run(monkeypatch, calls, stdout=stdout, before=create_artifact)

    r = client.post("/api/build", json={
        "board": BOARD, "project": PROJECT, "tests": True, "fresh": True})
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["ok"] is True
    assert body["returncode"] == 0
    assert body["timeout"] is False
    assert body["artifact"] == f"artifacts/{BOARD}.bin"
    assert body["size"] == 321
    assert body["chip"] == "esp32s3"
    assert body["elapsed"] >= 0
    assert body["lines"] == stdout.splitlines()
    assert body["cmd"] == [sys.executable, str(fake_repo["root"] / "make.py"),
                           "build", BOARD, "--project", PROJECT,
                           "--tests", "--fresh"]

    assert len(calls) == 1
    cmd, kwargs = calls[0]
    assert cmd == body["cmd"]
    assert kwargs["cwd"] == str(fake_repo["root"])
    assert kwargs["timeout"] == 600
    assert "shell" not in kwargs or kwargs["shell"] is False
    # stdout/stderr 一齊 capture（stderr 併入 stdout，保留次序）
    assert kwargs["stdout"] == subprocess.PIPE
    assert kwargs["stderr"] == subprocess.STDOUT
    assert kwargs["env"]["IDF_PATH"] == str(fake_repo["idf"])
    assert kwargs["env"]["PYTHONUTF8"] == "1"

    # 結果記入 app.state，info 帶得到
    assert client.app.state.esp.last_build["artifact"] == f"artifacts/{BOARD}.bin"
    info = client.get("/api/build/info").json()["data"]
    assert info["lastBuild"]["ok"] is True


def test_build_board_query_alias(client, fake_repo, monkeypatch):
    calls = []
    _mock_run(monkeypatch, calls, stdout="✔ 建置完成\n")
    r = client.post("/api/build", json={"board_query": "S3/ESP32-S3-Matrix"})
    assert r.status_code == 200, r.text
    cmd = calls[0][0]
    assert cmd[2:] == ["build", "S3/ESP32-S3-Matrix"]
    assert "--project" not in cmd
    assert r.json()["project"] is None


def test_build_failure_keeps_lines_and_no_artifact(client, fake_repo, monkeypatch):
    calls = []
    stdout = "→ 建置 ESP32-S3-Matrix\n✘ 建置失敗（3s）\n  error: something broke\n"
    _mock_run(monkeypatch, calls, returncode=2, stdout=stdout)
    r = client.post("/api/build", json={"board": BOARD})
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["ok"] is False
    assert body["returncode"] == 2
    assert body["artifact"] is None and body["size"] == 0
    assert any("error: something broke" in line for line in body["lines"])


def test_build_timeout_reports_partial_output(client, fake_repo, monkeypatch):
    def fake_run(cmd, **kwargs):
        raise subprocess.TimeoutExpired(cmd, 600, output="partial line\n")

    monkeypatch.setattr(server.subprocess, "run", fake_run)
    r = client.post("/api/build", json={"board": BOARD})
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["ok"] is False
    assert body["timeout"] is True
    assert body["returncode"] == -1
    joined = "\n".join(body["lines"])
    assert "partial line" in joined
    assert "600s" in joined


def test_build_missing_make_py(workdir, monkeypatch):
    empty = workdir / "empty-repo2"
    empty.mkdir()
    monkeypatch.setattr(server, "REPO_ROOT", empty)
    app = server.create_app(sim_root=workdir / "fs3")
    with TestClient(app) as c:
        r = c.post("/api/build", json={"board": BOARD})
    assert r.status_code == 400
    assert "make.py" in str(r.json()["detail"])


def test_build_requires_board_and_rejects_bad_chars(client):
    assert client.post("/api/build", json={}).status_code == 400
    bad = client.post("/api/build", json={"board": "-rf /"})
    assert bad.status_code == 400
    assert "格式" in str(bad.json()["detail"])


def test_build_already_running_returns_409(client):
    lock = client.app.state.esp.build_lock
    assert lock.acquire(blocking=False)
    try:
        r = client.post("/api/build", json={"board": BOARD})
    finally:
        lock.release()
    assert r.status_code == 409


def test_build_recorded_in_activity(client, fake_repo, monkeypatch):
    """任務要求：build 完成後要入 `state.activity`（UI 活動面板／AI 都睇到）。"""
    calls = []
    _mock_run(monkeypatch, calls, stdout="✔ 建置完成\n")
    assert client.post("/api/connect", json={"sim": True}).status_code == 200
    assert client.post("/api/build", json={"board": BOARD}).status_code == 200
    events = client.get("/api/activity?n=50").json()["events"]
    builds = [e for e in events if e.get("action") == "build"]
    assert builds, f"activity 冇 build：{events}"
    last = builds[-1]
    assert last["actor"] == "ui"
    assert last["board"] == BOARD
    assert last["ok"] is True
    assert last["returncode"] == 0


# ── artifact endpoint（一鍵 OTA 用）────────────────────

def test_find_artifact_from_make_output(fake_repo):
    art = fake_repo["root"] / "artifacts" / f"{BOARD}.bin"
    art.parent.mkdir(parents=True)
    art.write_bytes(b"x" * 7)
    found = server._find_artifact(
        [f"  產物    artifacts/{BOARD}.bin，+12 B vs 上一顆\n"], BOARD, started=0)
    assert found["path"] == f"artifacts/{BOARD}.bin"
    assert found["size"] == 7


def test_find_artifact_newest_file_fallback(fake_repo):
    """archive 關掉／output 冇「產物」行時，用 artifacts/ 下新過 build 開始時間的最新 .bin。"""
    import time as _time
    art = fake_repo["root"] / "artifacts" / "Whatever.bin"
    art.parent.mkdir(parents=True)
    art.write_bytes(b"xyz")
    found = server._find_artifact([], "NO-SUCH-BOARD", started=_time.time() - 1)
    assert found["path"] == "artifacts/Whatever.bin"
    assert server._find_artifact([], BOARD, started=_time.time() + 60) is None


def test_artifact_endpoint_serves_bin(client, fake_repo):
    art = fake_repo["root"] / "artifacts" / f"{BOARD}.bin"
    art.parent.mkdir(parents=True)
    art.write_bytes(b"BIN-DATA")
    r = client.get(f"/api/build/artifact?path=artifacts/{BOARD}.bin")
    assert r.status_code == 200, r.text
    assert r.content == b"BIN-DATA"
    assert r.headers["content-type"] == "application/octet-stream"


def test_artifact_endpoint_rejects_outside_and_non_bin(client, fake_repo):
    root = fake_repo["root"]
    (root / "artifacts").mkdir()
    (root / "secret.bin").write_bytes(b"secret")
    (root / "artifacts/notes.txt").write_text("x", encoding="utf-8")

    outside = client.get("/api/build/artifact?path=secret.bin")
    assert outside.status_code == 400
    traversal = client.get("/api/build/artifact?path=../secret.bin")
    assert traversal.status_code == 400
    txt = client.get("/api/build/artifact?path=artifacts/notes.txt")
    assert txt.status_code == 400
    missing = client.get("/api/build/artifact?path=artifacts/nope.bin")
    assert missing.status_code == 404


# ── AI 能力清單 ────────────────────────────────────────

def test_build_endpoints_documented_in_ai(client):
    paths = [item["path"] for item in client.get("/api/ai").json()["endpoints"]]
    assert "/api/build" in paths
    assert "/api/build/info" in paths
    assert any(p.startswith("/api/build/artifact") for p in paths)
