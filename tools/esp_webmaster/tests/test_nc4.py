import struct

from nc4 import Demux, decode_str_u16, pack, str_u16
from link import BaseLink, LinkError


def test_pack_roundtrip():
    raw = pack(0x1101, str_u16("sys"))
    d = Demux()
    ev = d.feed(raw)
    assert ev == [("frame", (0x1101, str_u16("sys")))]


def test_fake_sof_then_real_frame():
    d = Demux()
    ev = d.feed(b"NC" + b"\x00" * 3 + pack(0x100F))
    frames = [e for e in ev if e[0] == "frame"]
    assert len(frames) == 1
    assert frames[0][1][0] == 0x100F


def test_bad_crc_keeps_scanning():
    good = pack(0x100F)
    bad = bytearray(good)
    bad[-1] ^= 0xFF
    d = Demux()
    ev = d.feed(bytes(bad) + good)
    frames = [e for e in ev if e[0] == "frame"]
    assert len(frames) == 1
    assert frames[0][1][0] == 0x100F


def test_split_frame():
    raw = pack(0x1101, b"hello")
    d = Demux()
    assert d.feed(raw[:5]) == []
    ev = d.feed(raw[5:])
    assert ev[0][1][0] == 0x1101
    assert ev[0][1][1] == b"hello"


def test_log_line_mixed_with_frame():
    d = Demux()
    ev = d.feed(b"\x1eI (123) boot: hi\n" + pack(0x1102, str_u16('{"a":1}')))
    assert [e[0] for e in ev] == ["line", "frame"]
    assert ev[0][1] == "I (123) boot: hi"
    assert decode_str_u16(ev[1][1][1]) == '{"a":1}'


def test_str_u16_truncates_at_limit():
    assert decode_str_u16(str_u16("x" * 300)) == "x" * 200


class _FakeDevice:
    """收到 0x1101 就回 0x1102 + JSON；收到 0x100F 唔回。"""

    def __init__(self):
        self.seen = []

    def handle(self, cmd, payload):
        self.seen.append((cmd, payload))
        if cmd == 0x1101:
            return [("frame", (0x1102, str_u16('{"ok":1}')))]
        return []


class _FakeLink(BaseLink):
    def __init__(self, device):
        super().__init__()
        self.device = device
        self._closed = False

    @property
    def connected(self):
        return not self._closed

    def send(self, cmd, payload=b""):
        self._dispatch(self.device.handle(cmd, payload))

    def close(self):
        self._closed = True


def test_request_matches_expected_cmd():
    dev = _FakeDevice()
    link = _FakeLink(dev)
    cmd, payload = link.request(0x1101, b"", expect={0x1102}, timeout=1.0)
    assert cmd == 0x1102
    assert decode_str_u16(payload) == '{"ok":1}'


def test_request_timeout():
    link = _FakeLink(_FakeDevice())
    try:
        link.request(0x100F, b"", expect={0x1102}, timeout=0.05)
    except LinkError as e:
        assert "timeout" in str(e)
    else:
        raise AssertionError("should timeout")


def test_unmatched_frame_still_goes_to_on_frame():
    seen = []
    link = _FakeLink(_FakeDevice())
    link.on_frame = lambda cmd, payload: seen.append(cmd)
    link.request(0x1101, b"", expect={0x1102}, timeout=1.0)
    assert 0x1102 in seen
