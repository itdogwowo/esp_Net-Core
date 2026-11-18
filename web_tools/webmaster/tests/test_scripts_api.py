"""`/api/scripts` 同 `/api/scripts/one` 的 API 測試（用臨時 repo 根）。

2026-11-18 起目錄結構係：
  <repo>/web_tools/{web.sh, WebMaster.bat, …}   ← 網頁工具腳本
  <repo>/{make.bat, build.sh, …}                ← 建置腳本
所以「啟動」清單要掃兩條根，`rel` 由 repo 根起算。
"""
import pytest
from fastapi.testclient import TestClient

import server

WEB_SH = """#!/usr/bin/env bash
# 一鍵啟動 ESP WebMaster
#   ./web.sh --sim
set -euo pipefail
echo hi
"""

WEB_BAT = """@echo off
REM  ESP WebMaster - Windows launcher
REM    WebMaster.bat --serial COM13
setlocal
"""

MAKE_BAT = """@echo off
REM  build helper
REM    make.bat build s3
setlocal
"""


@pytest.fixture()
def fake_repo(workdir, monkeypatch):
    """假 repo 根：web_tools/（工具）＋ 根（建置），同真實佈局一樣。"""
    root = workdir / "repo"
    tools = root / "web_tools"
    tools.mkdir(parents=True)
    (tools / "web.sh").write_text(WEB_SH, encoding="utf-8")
    (tools / "WebMaster.bat").write_text(WEB_BAT, encoding="utf-8")
    (tools / "notes.md").write_text("# not a script\n", encoding="utf-8")
    (root / "make.bat").write_text(MAKE_BAT, encoding="utf-8")
    monkeypatch.setattr(server, "REPO_ROOT", root)
    return root


def test_scripts_api_shape(fake_repo, workdir):
    app = server.create_app(sim_root=workdir / "fs")
    with TestClient(app) as c:
        r = c.get("/api/scripts")
        assert r.status_code == 200, r.text
        body = r.json()
        assert body["ok"] is True
        assert [c["key"] for c in body["categories"]] == [
            "webmaster", "build", "device", "ai"]
        names = sorted(s["name"] for s in body["data"])
        assert names == ["WebMaster.bat", "make.bat", "web.sh"]
        assert "notes.md" not in names
        assert body["counts"]["webmaster"] == 2
        assert body["counts"]["build"] == 1
        assert body["roots"] == ["web_tools", "repo"]
        for s in body["data"]:
            assert s["platform"] in body["platforms"]
            assert s["platforms"]
            # rel 由 repo 根起算，可直接貼落終端
            assert s["rel"] in ("web_tools/web.sh", "web_tools/WebMaster.bat", "make.bat")
            assert s["location"] in ("web_tools", ".")
            if s["location"] == "web_tools":
                assert s["rel"].startswith("web_tools/")


def test_scripts_one_ok(fake_repo, workdir):
    app = server.create_app(sim_root=workdir / "fs2")
    with TestClient(app) as c:
        r = c.get("/api/scripts/one", params={"rel": "web_tools/web.sh"})
        assert r.status_code == 200, r.text
        d = r.json()["data"]
        assert d["text"].startswith("#!/usr/bin/env bash")
        assert d["rel"] == "web_tools/web.sh"
        # 亦可以用 name= 由 server 自己找
        r2 = c.get("/api/scripts/one", params={"name": "web.sh"})
        assert r2.status_code == 200
        assert r2.json()["data"]["rel"] == "web_tools/web.sh"
        # repo 根的建置腳本照樣讀得到
        r3 = c.get("/api/scripts/one", params={"rel": "make.bat"})
        assert r3.status_code == 200
        assert "build helper" in r3.json()["data"]["text"]


@pytest.mark.parametrize("rel", [
    "../web.sh", "../repo/web_tools/web.sh", "sub/web.sh", "web_tools/../make.bat",
    "/etc/passwd", "web_tools/notes.md", "notes.md", "a/b/c.sh",
])
def test_scripts_one_rejects(fake_repo, workdir, rel):
    app = server.create_app(sim_root=workdir / "fs3")
    with TestClient(app) as c:
        r = c.get("/api/scripts/one", params={"rel": rel})
        assert r.status_code == 404


def test_scripts_one_requires_a_target(fake_repo, workdir):
    """`rel` 同 `name` 都冇 → 400（唔好靜靜回全部）。"""
    app = server.create_app(sim_root=workdir / "fs5")
    with TestClient(app) as c:
        assert c.get("/api/scripts/one").status_code == 400


def test_scripts_api_survives_missing_root(workdir, monkeypatch):
    """repo 根唔存在（打包／搬走）時要回空清單，唔可以 500。"""
    monkeypatch.setattr(server, "REPO_ROOT", workdir / "nope")
    app = server.create_app(sim_root=workdir / "fs4")
    with TestClient(app) as c:
        r = c.get("/api/scripts")
        assert r.status_code == 200
        assert r.json()["data"] == []
