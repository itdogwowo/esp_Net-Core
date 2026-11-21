"""前端：Build 卡／8×8 燈泡模擬器／Show 全流程的 id 同模組載入次序。

server 側 pytest 只驗 API；拆檔之後最容易出錯的是「index.html 漏載模組」或
「JS 引用唔存在的 id」——瀏覽器只會靜靜 `null`。呢度用靜態掃描鎖住：

1. index.html 有新卡的全部 id（Build 卡、modeMatrix／modeWhite、flow 按鈕）；
2. `61-build.js` 有載入，而且喺 `60-flash.js` 之後、`96-mode.js` 之前；
3. white mix 預設 80、range 0..100；`96-mode.js` 有 drawMatrix／0xF06B；
4. `61-build.js` 用 `/api/build`、`/api/build/info`、`/api/build/artifact`；
5. `75-shell.js` 入 flash／mode 頁有 hook（入頁讀 info／重畫 matrix）。
"""
import re
from pathlib import Path

import pytest

BASE = Path(__file__).resolve().parents[1]
STATIC = BASE / "static"
JS = STATIC / "js"

BUILD_IDS = [
    "buildCard", "buildBoard", "buildProject", "buildTests", "buildFresh",
    "btnBuild", "btnBuildReload", "btnBuildOta", "buildState",
    "buildLog", "buildArtifact", "buildIdf",
]
MATRIX_IDS = [
    "modeMatrix", "modeWhite", "modeWhiteVal", "modeGlow",
    "btnModeRead", "modeFrameInfo", "btnModeMatrixZero",
]
FLOW_IDS = [
    "showFlowCard", "flowState", "flowLog", "btnFlowLoad", "btnFlowPreview",
    "btnFlowBuild", "btnFlowOta", "btnFlowRead", "btnFlowReset",
]


def html() -> str:
    return (STATIC / "index.html").read_text(encoding="utf-8")


def js(name: str) -> str:
    return (JS / name).read_text(encoding="utf-8")


def script_srcs() -> list[str]:
    return re.findall(r'<script src="/static/js/([^"?]+)', html())


# ── id 存在 ────────────────────────────────────────────

@pytest.mark.parametrize("ident", BUILD_IDS + MATRIX_IDS + FLOW_IDS)
def test_new_card_ids_exist_in_index(ident):
    assert re.search(rf'\bid="{ident}"', html()), f"index.html 缺 #{ident}"


def test_id_list_has_no_duplicates():
    page = html()
    for ident in BUILD_IDS + MATRIX_IDS + FLOW_IDS:
        assert page.count(f'id="{ident}"') == 1, f"#{ident} 重複"


def test_build_card_is_in_flash_view():
    page = html()
    flash = page.split('<section class="view" data-view="flash">', 1)[1]
    flash = flash.split("</section>", 1)[0]
    assert 'id="buildCard"' in flash
    assert "make.py build" in flash


def test_matrix_card_is_in_mode_view():
    page = html()
    mode = page.split('<section class="view" data-view="mode">', 1)[1]
    mode = mode.split('<section class="view" data-view="wave">', 1)[0]
    assert 'id="modeMatrix"' in mode
    assert 'id="modeWhite"' in mode
    assert 'id="showFlowCard"' in mode
    # 1D strip 保留
    assert 'id="modeStrip"' in mode


# ── 元素屬性 ───────────────────────────────────────────

def test_mode_white_is_range_0_to_100_default_80():
    page = html()
    match = re.search(r'<input[^>]*id="modeWhite"[^>]*>', page)
    assert match, "搵唔到 #modeWhite"
    tag = match.group(0)
    assert 'type="range"' in tag
    assert 'min="0"' in tag
    assert 'max="100"' in tag
    assert 'value="80"' in tag


def test_mode_matrix_is_canvas_and_white_default_label_80():
    page = html()
    match = re.search(r'<canvas[^>]*id="modeMatrix"[^>]*>', page)
    assert match, "搵唔到 canvas#modeMatrix"
    assert '<span id="modeWhiteVal">80</span>' in page


def test_flow_buttons_cover_full_pipeline():
    page = html()
    mode = page.split('<section class="view" data-view="mode">', 1)[1]
    for word in ("載入 /pixel", "預覽 8×8", "Build", "OTA", "讀 device frame"):
        assert word in mode, f"全流程卡缺「{word}」"


# ── 模組載入次序 ───────────────────────────────────────

def test_build_module_loaded_in_order():
    srcs = script_srcs()
    assert "60-flash.js" in srcs and "61-build.js" in srcs and "96-mode.js" in srcs
    assert srcs.index("61-build.js") == srcs.index("60-flash.js") + 1, \
        "61-build.js 要緊接 60-flash.js 載入"
    assert srcs.index("61-build.js") < srcs.index("96-mode.js"), \
        "96-mode.js 用 BuildApi，所以 61-build.js 要先載入"


def test_new_modules_declare_strict_mode():
    for name in ("61-build.js", "96-mode.js"):
        assert '"use strict";' in js(name)


# ── JS 內容 ────────────────────────────────────────────

def test_build_module_calls_build_api():
    text = js("61-build.js")
    for token in ('"/api/build/info"', '"/api/build"', '"/api/build/artifact',
                  '"/api/firmware?', "BuildApi"):
        assert token in text, f"61-build.js 缺 {token}"
    assert "method: \"POST\"" in text


def test_mode_module_has_matrix_white_and_device_frame():
    text = js("96-mode.js")
    assert "function drawMatrix" in text
    assert "0xF06B" in text and "0xF06C" in text
    assert "applyWhite" in text
    # 任務指定的公式：out*(100-w)/100 + 255*w/100
    assert "* (100 - w) / 100 + 255 * w / 100" in text
    assert "modeMatrix" in text
    assert "btnModeRead" in text
    # 全流程用新 Build API／現有 firmware endpoint
    assert "BuildApi.build" in text
    assert "BuildApi.ota" in text


def test_shell_hooks_build_and_mode_views():
    text = js("75-shell.js")
    assert "BuildApi.loadInfo" in text
    assert "ModeSim.onShow" in text
