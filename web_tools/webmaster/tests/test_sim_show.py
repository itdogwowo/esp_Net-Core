"""SimDevice 的 Show 指令（0xF06B／F06D／F06F）契約。

唔使板：模擬器回同一條 JSON 契約，Web UI 就可以完整試 live frame／切 mode／reload。
"""
from __future__ import annotations

import json
import struct

from sim import SimDevice


def _reply(dev: SimDevice, cmd: int, payload: bytes = b"") -> tuple[int, bytes]:
    events = dev.handle(cmd, payload)
    for kind, value in events:
        if kind == "frame":
            return value
    raise AssertionError(f"0x{cmd:04X} 冇 frame 回覆：{events}")


def _json_payload(payload: bytes) -> dict:
    n = struct.unpack_from("<H", payload, 0)[0]
    return json.loads(payload[2:2 + n].decode("utf-8"))


def test_sim_showframe_contract(tmp_path):
    dev = SimDevice(tmp_path / "sim")
    cmd, payload = _reply(dev, 0xF06B)
    assert cmd == 0xF06C
    obj = _json_payload(payload)
    assert obj["px"] == 64
    assert obj["mode"] == "breathe"
    assert obj["white"] == 0
    raw = bytes.fromhex(obj["hex"])
    assert len(raw) == 64 * 4
    # 呼吸值 0..23；第一幀是 sine 中點（12），不應是 80% 白的 204。
    vals = [raw[i] for i in range(0, len(raw), 4)]
    assert 0 <= min(vals) <= max(vals) <= 24
    assert obj["seq"] == 1

    _, payload2 = _reply(dev, 0xF06B)
    obj2 = _json_payload(payload2)
    assert obj2["seq"] == 2


def test_sim_showmode_and_reload(tmp_path):
    dev = SimDevice(tmp_path / "sim")
    cmd, payload = _reply(dev, 0xF06D, b"\x02")
    assert cmd == 0xF06E
    obj = _json_payload(payload)
    assert obj["ok"] == 1 and obj["queued"] == 2

    cmd, payload = _reply(dev, 0xF06F)
    assert cmd == 0xF070
    obj = _json_payload(payload)
    assert obj["queued"] == "reload"
