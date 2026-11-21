"""前端模組的完整性測試（拆檔之後最容易出錯的地方）。

拆成 static/js/*.js 之後，風險變成「index.html 漏載某個檔」或「引用了不存在的
DOM id」。呢類錯誤在瀏覽器只會 `undefined is not a function`，server 側冇感覺，
所以用 AST 掃描鎖住：

1. index.html 的 `<script src>` 次序 == 應載入清單（而且 core 一定第一、app 最後）；
2. 每個 js 檔都有 `"use strict"`，且 `node --check` 通過（語法）；
3. JS 用到的 `$("#id")` 全部存在於 index.html（或係動態建立的元素）；
4. 沒有殘留的 `#launch` 頁引用。
"""
import json
import re
import shutil
import subprocess
from pathlib import Path

import pytest

BASE = Path(__file__).resolve().parents[1]
STATIC = BASE / "static"
JS_DIR = STATIC / "js"

EXPECTED = [
    "00-core.js",
    "10-connection.js",
    "20-terminal.js",
    "30-status.js",
    "40-files.js",
    "50-drive.js",
    "60-flash.js",
    "61-build.js",
    "65-browser-flash.js",
    "66-flash-request.js",
    "70-console.js",
    "75-shell.js",
    "80-ide.js",
    "85-container.js",
    "90-env.js",
    "95-wave.js",
    "96-mode.js",
    "99-app.js",
]

# 這些 id 由 JS 動態建立（唔喺 index.html 的靜態 DOM 裡）
DYNAMIC_IDS = {
    "toasts",             # 由 HTML 靜態提供，但保險起見再列一次
}


def html():
    return (STATIC / "index.html").read_text(encoding="utf-8")


def script_srcs():
    """index.html 載入的模組次序（`?v=` 版本字串唔計）。"""
    return re.findall(r'<script src="/static/js/([^"?]+)', html())


def test_js_dir_has_exactly_expected_modules():
    actual = sorted(p.name for p in JS_DIR.glob("*.js"))
    assert actual == EXPECTED, f"模組清單唔一致：{actual}"


def test_index_html_loads_modules_in_expected_order():
    srcs = script_srcs()
    assert srcs == EXPECTED, f"index.html 載入次序：{srcs}"
    # 舊的單檔 app.js 唔應該再被引用
    assert "/static/app.js" not in html()
    assert not (STATIC / "app.js").exists()


def test_core_is_first_and_app_is_last():
    srcs = script_srcs()
    assert srcs[0] == "00-core.js"
    assert srcs[-1] == "99-app.js"


def test_every_module_declares_strict_mode():
    for name in EXPECTED:
        text = (JS_DIR / name).read_text(encoding="utf-8")
        assert '"use strict";' in text, f"{name} 缺 use strict"


@pytest.mark.skipif(shutil.which("node") is None, reason="需要 node 檢查 JS 語法")
def test_every_module_parses():
    for name in EXPECTED:
        r = subprocess.run(["node", "--check", str(JS_DIR / name)],
                           capture_output=True, text=True)
        assert r.returncode == 0, f"{name} 語法錯：{r.stderr[:400]}"


def test_dollar_ids_exist_in_html():
    """JS 用 `$("#foo")` 找嘅 id，要真係喺 index.html（否則永遠 null）。"""
    page = html()
    have = set(re.findall(r'\bid="([^"]+)"', page))
    missing = {}
    for name in EXPECTED:
        text = (JS_DIR / name).read_text(encoding="utf-8")
        for m in re.finditer(r'\$\("#([A-Za-z0-9_-]+)"\)', text):
            ident = m.group(1)
            if ident in have or ident in DYNAMIC_IDS:
                continue
            missing.setdefault(ident, set()).add(name)
    assert not missing, "JS 引用咗 HTML 冇的 id：" + json.dumps(
        {k: sorted(v) for k, v in missing.items()}, ensure_ascii=False, indent=1)


def test_no_launch_page_leftovers():
    """#launch 頁已刪：HTML／JS 都唔應該再有它的痕跡。"""
    page = html()
    assert "data-view=\"launch\"" not in page
    assert "launch" not in page.lower().replace("launched", "")
    for name in EXPECTED:
        text = (JS_DIR / name).read_text(encoding="utf-8")
        assert "launchRender" not in text and "launchCard" not in text, name
        assert "loadScripts" not in text, name


def test_ws_and_api_paths_are_absolute():
    """REST／WS 路徑一定要係 /api 或 /ws 開頭（file:// 或子路徑部署都唔會錯）。"""
    for name in EXPECTED:
        text = (JS_DIR / name).read_text(encoding="utf-8")
        for m in re.finditer(r'\bapi\("([^"]+)"', text):
            assert m.group(1).startswith("/api"), f"{name}: {m.group(1)}"
