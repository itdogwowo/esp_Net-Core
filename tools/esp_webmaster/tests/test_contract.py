"""C firmware ↔ UI 指令碼一致性（唔需要板、唔需要 IDF）。

防止其中一邊改咗 0x20xx、另一邊唔知。呢個 test 只讀原始碼做靜態比對。
"""
import re
from pathlib import Path

import files

ROOT = Path(__file__).resolve().parents[3]
FW = ROOT / "firmware/components/action/file_action/nc_file_action.c"


def _fw_defines() -> dict[str, int]:
    src = FW.read_text(encoding="utf-8")
    return {
        name: int(hex_id, 16)
        for name, hex_id in re.findall(
            r"#define\s+(FILE_[A-Z_]+)\s+0x([0-9A-Fa-f]+)", src
        )
    }


def test_file_cmd_ids_match():
    fw = _fw_defines()
    ui = {k: v for k, v in vars(files).items() if k.startswith("CMD_FILE_")}
    # UI 的 CMD_FILE_* 名稱去掉前綴後，應該同 firmware 的 FILE_* 一一對應
    fw_ids = set(fw.values())
    ui_ids = set(ui.values())
    missing = ui_ids - fw_ids
    extra = fw_ids - ui_ids
    assert not missing, f"UI 有、firmware 冇：{sorted(hex(x) for x in missing)}"
    assert not extra, f"firmware 有、UI 冇：{sorted(hex(x) for x in extra)}"


def test_all_file_handlers_are_registered():
    src = FW.read_text(encoding="utf-8")
    table = src.split("s_file_actions[] = {", 1)[1].split("};", 1)[0]
    registered = set(re.findall(r"\{\s*(FILE_[A-Z_]+)\s*,", table))
    requests = {
        "FILE_BEGIN", "FILE_CHUNK", "FILE_END", "FILE_QUERY", "FILE_READ",
        "FILE_CONFIRM", "FILE_DELETE", "FILE_UNDO", "FILE_SCAN",
    }
    assert requests <= registered, f"未註冊：{sorted(requests - registered)}"


def test_firmware_has_no_duplicate_cmd():
    src = FW.read_text(encoding="utf-8")
    ids = [int(x, 16) for x in re.findall(r"#define\s+FILE_[A-Z_]+\s+0x([0-9A-Fa-f]+)", src)]
    assert len(ids) == len(set(ids)), "FILE_* 有重複指令碼"
