"""「真實板子」偵測的測試（`link.is_likely_board` / `link.find_board_port`）。

背景：Windows 上 `comports()` 會同時列出 Intel AMT SOL、藍牙 SPP 這些系統埠。
如果 `autoconnect: "auto"` 揀錯（例如揀到 COM3 Intel AMT），就會出現
「连線成功但 STATUS 一直 timeout」，非常難查。所以白名單要鎖住。
"""
import pytest

import link


def port(device, desc="", vid=None, pid=None, hwid=""):
    return {"device": device, "description": desc, "hwid": hwid or f"USB VID:PID={vid}",
            "vid": vid, "pid": pid}


# ── 應該當係板 ──────────────────────────────────────────

@pytest.mark.parametrize("vid", [0x303A, 0x10C4, 0x1A86, 0x0403, 0x1B4F, 0x2341])
def test_known_usb_serial_vids_are_boards(vid):
    assert link.is_likely_board(port("COM9", "USB Serial Device", vid=vid)) is True


def test_espressif_native_usb_is_board():
    # S3／C3 的 USB-Serial-JTAG：VID 0x303A
    p = port("COM20", "USB Serial Device (COM20)", vid=0x303A, pid=0x1001)
    assert link.is_likely_board(p) is True


def test_unknown_vid_but_esp_in_description():
    assert link.is_likely_board(port("COM7", "ESP32-S3 devkit", vid=0x9999)) is True


# ── 唔應該當係板 ────────────────────────────────────────

@pytest.mark.parametrize("desc", [
    "Intel(R) Active Management Technology - SOL (COM3)",
    "Standard Serial over Bluetooth link (COM32)",
    "藍牙序列埠 (COM33)",
])
def test_system_ports_are_not_boards(desc):
    assert link.is_likely_board(port("COM3", desc, vid=None)) is False


def test_port_without_vid_is_not_board():
    # 真板一定係 USB 序列（有 VID）；冇 VID 的係 PCI／藍牙／虛擬埠
    assert link.is_likely_board(port("COM1", "Communications Port", vid=None)) is False


def test_bluetooth_wins_over_vid():
    """就算有 VID，描述講明係藍牙都唔算板。"""
    assert link.is_likely_board(port("COM9", "Standard Serial over Bluetooth", vid=0x303A)) is False


# ── find_board_port ─────────────────────────────────────

def test_find_returns_none_when_no_board(monkeypatch):
    monkeypatch.setattr(link, "list_ports", lambda: [
        port("COM3", "Intel(R) AMT - SOL", vid=None),
        port("COM32", "Bluetooth link", vid=None),
    ])
    assert link.find_board_port() is None


def test_find_prefers_espressif_over_other_vids(monkeypatch):
    monkeypatch.setattr(link, "list_ports", lambda: [
        port("COM5", "USB Serial", vid=0x1A86),     # CH340
        port("COM20", "USB Serial Device", vid=0x303A),  # Espressif
        port("COM3", "Intel AMT", vid=None),
    ])
    assert link.find_board_port() == "COM20"


def test_find_honours_prefer_port(monkeypatch):
    monkeypatch.setattr(link, "list_ports", lambda: [
        port("COM5", "USB Serial", vid=0x1A86),
        port("COM20", "USB Serial Device", vid=0x303A),
    ])
    assert link.find_board_port(prefer="COM5") == "COM5"


def test_find_ignores_prefer_when_not_a_board(monkeypatch):
    """prefer_port 指向系統埠（唔係板）時要忽略，唔可以照連。"""
    monkeypatch.setattr(link, "list_ports", lambda: [
        port("COM3", "Intel(R) AMT - SOL", vid=None),
        port("COM20", "USB Serial Device", vid=0x303A),
    ])
    assert link.find_board_port(prefer="COM3") == "COM20"


def test_find_single_board(monkeypatch):
    monkeypatch.setattr(link, "list_ports", lambda: [
        port("COM20", "USB Serial Device", vid=0x303A),
        port("COM32", "Bluetooth", vid=None),
    ])
    assert link.find_board_port() == "COM20"
