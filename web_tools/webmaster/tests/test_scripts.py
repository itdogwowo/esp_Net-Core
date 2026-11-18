"""啟動腳本清單／分類／讀取的測試。

重點：
- 分類層級（webmaster → build → device → ai）係 UI 的骨架，唔可以亂；
- 平台判定（.ps1/.bat → Windows、.command → macOS、.sh → macOS＋Linux）；
- 讀取防穿越（`../x.sh`、絕對路徑、symlink 一律拒絕）；
- 未知腳本都有摘要（唔會回空字串）。
"""
import json
import os

import pytest

import scripts

# ── 樣本腳本：真檔內容的一小撮，足夠驗啟發式 ──

PS1 = r"""<#
  ESP WebMaster — Windows 啟動
  用法：
    .\web.ps1 -Serial COM13
#>
[CmdletBinding()]
param(
  [switch]$Sim,
  [string]$Serial,
  [int]$Port = 8010
)
$ErrorActionPreference = 'Stop'
"""

BAT = r"""@echo off
REM  ESP WebMaster - Windows one-click launcher
REM
REM  Usage:
REM    WebMaster.bat --serial COM13
REM    WebMaster.bat --port 8010
setlocal EnableExtensions
"""

SH = r"""#!/usr/bin/env bash
# 一鍵啟動 ESP WebMaster（repo 根）。
#
#   ./web.sh --sim
#   ./web.sh --serial /dev/cu.usbmodemXXXX --port 8011
set -euo pipefail
"""

CMD = """#!/bin/bash
# macOS 雙擊啟動。
cd "$(dirname "$0")" || exit 1
./web.sh "$@"
"""


def write(root, name, text):
    (root / name).write_text(text, encoding="utf-8")


@pytest.fixture()
def repo(workdir):
    """真實佈局：啟動腳本喺 web_tools/，建置腳本喺 repo 根。"""
    tools = workdir / "web_tools"
    tools.mkdir()
    write(tools, "web.ps1", PS1)
    write(tools, "WebMaster.bat", BAT)
    write(tools, "web.sh", SH)
    write(tools, "WebMaster.command", CMD)
    write(workdir, "make.bat", "@echo off\nREM  Build helper\nREM  make.bat build s3\n")
    write(workdir, "README.md", "# hi\n")          # 唔係腳本
    write(tools, "_internal.sh", "#!/bin/sh\n")     # 內部腳本，唔該出現
    write(workdir, "notes.txt", "hello\n")
    return workdir


def roots(repo):
    return [repo / "web_tools", repo]


def by_name(data):
    return {s["name"]: s for s in data}


def test_discover_classifies_and_orders(repo):
    data = scripts.discover(roots(repo))
    names = [s["name"] for s in data]
    assert "README.md" not in names and "notes.txt" not in names
    assert "_internal.sh" not in names           # 前綴 `_` 唔當入口
    got = by_name(data)

    # 分類：web.sh／run_web.sh 唔可以俾 build 搶走
    assert got["web.sh"]["category"] == "webmaster"
    assert got["WebMaster.bat"]["category"] == "webmaster"
    assert got["make.bat"]["category"] == "build"

    # 次序＝CATEGORIES 次序（webmaster 先，build 後），同組內平台次序
    cats = [s["category"] for s in data]
    assert cats == sorted(cats, key=lambda c: [k for k, *_ in scripts.CATEGORIES].index(c))
    assert names[0] in ("web.ps1", "WebMaster.bat", "web.sh", "WebMaster.command")


def test_platform_detection(repo):
    got = by_name(scripts.discover(roots(repo)))
    assert got["web.ps1"]["platform"] == "windows"
    assert got["WebMaster.bat"]["platform"] == "windows"
    assert got["WebMaster.command"]["platform"] == "macos"
    assert got["make.bat"]["platform"] == "windows"
    # .sh 同時係 macOS 同 Linux
    assert got["web.sh"]["platform"] == "macos"
    assert set(got["web.sh"]["platforms"]) == {"macos", "linux"}


def test_summary_and_args(repo):
    got = by_name(scripts.discover(roots(repo)))
    for s in got.values():
        assert s["summary"], f"{s['name']} 冇摘要"
        assert len(s["summary"]) <= 200
    # .ps1：只抽 param() 裡面嘅參數（唔會掃到內部旗標）
    assert got["web.ps1"]["args"] == ["-Sim", "-Serial", "-Port"]
    # .bat：抽註解區的 --flag
    assert "--serial" in got["WebMaster.bat"]["args"]
    assert "--port" in got["WebMaster.bat"]["args"]
    # 已知腳本用中文摘要（UI 係中文）
    assert "雙擊" in got["WebMaster.bat"]["summary"]


def test_bat_preamble_does_not_hide_comments(repo):
    """`@echo off` 喺第一行；用法說明喺後面 —— 要讀得到（踩過）。"""
    write(repo / "web_tools", "tool.bat", "@echo off\nREM  build helper\nREM  tool.bat --all\n")
    got = by_name(scripts.discover(roots(repo)))
    assert got["tool.bat"]["summary"] == "build helper"
    assert "--all" in got["tool.bat"]["args"]


def test_read_roundtrip(repo):
    got = scripts.read(roots(repo), "web.sh")
    assert got["text"].startswith("#!/usr/bin/env bash")
    assert got["rel"] == "web_tools/web.sh"
    assert got["truncated"] is False
    # 用磁碟上的實際位元組：Windows 的 write_text 會把 \n 轉成 \r\n
    assert got["size"] == (repo / "web_tools" / "web.sh").stat().st_size


@pytest.mark.parametrize("name", [
    "../web.sh", "sub/web.sh", "..", ".", "", "web.sh\x00", "/etc/passwd",
    "web.sh.exe", "README.md", "notes.txt",
])
def test_read_rejects_traversal_and_non_scripts(repo, name):
    with pytest.raises(ValueError):
        scripts.read(roots(repo), name)


def test_read_rejects_symlink(repo):
    link = repo / "web_tools" / "link.sh"
    try:
        if not hasattr(os, "symlink"):
            pytest.skip("os.symlink 唔支援")
        link.symlink_to(repo / "web_tools" / "web.sh")
    except (OSError, NotImplementedError):
        pytest.skip("建立 symlink 需要管理員權限（Windows）")
    with pytest.raises(ValueError):
        scripts.read(roots(repo), "link.sh")


def test_rel_paths_are_repo_root_relative(repo):
    """`rel` 要可以直接貼落終端（由 repo 根起算），唔係只有檔名。"""
    got = by_name(scripts.discover(roots(repo)))
    assert got["web.sh"]["rel"] == "web_tools/web.sh"
    assert got["web.sh"]["location"] == "web_tools"
    assert got["make.bat"]["rel"] == "make.bat"
    assert got["make.bat"]["location"] == "."
    # 提示也用相對路徑
    assert "web_tools" in got["web.sh"]["hint"]


def test_discover_missing_root_is_empty(workdir):
    assert scripts.discover([workdir / "nope"]) == []


def test_categories_cover_known_names():
    """分類關鍵字唔可以互相蓋過：run_web.sh 一定要係 webmaster（唔係 build）。"""
    assert scripts.category_of("run_web.sh", "run_web.sh") == "webmaster"
    assert scripts.category_of("web.ps1", "web.ps1") == "webmaster"
    assert scripts.category_of("build.sh", "build.sh") == "build"
    assert scripts.category_of("make.bat", "make.bat") == "build"
    assert scripts.category_of("flash_helper.sh", "flash_helper.sh") == "build"
    assert scripts.category_of("serial_probe.sh", "serial_probe.sh") == "device"
    assert scripts.category_of("mcp_server.sh", "mcp_server.sh") == "ai"
    assert scripts.category_of("whatever.sh", "whatever.sh") == "device"
