import hashlib
import struct

import pytest

import nc4
import ota
from link import SimLink
from sim import SimDevice


@pytest.fixture()
def sim(workdir):
    dev = SimDevice(workdir / "fs")
    return dev, SimLink(dev)


def test_ota_writes_firmware(sim, workdir):
    dev, link = sim
    data = bytes(range(256)) * 30          # 7.5 KiB
    seen = []
    res = ota.ota_update(link, data, chunk_size=1024, apply=True,
                         progress=lambda d, t: seen.append((d, t)))
    assert res["ok"] and res["applied"]
    assert (workdir / "fs" / "firmware.bin").read_bytes() == data
    assert seen[-1] == (len(data), len(data))


def test_ota_sha_mismatch(sim):
    dev, link = sim
    data = b"hello"
    bad = hashlib.sha256(b"other").digest()
    payload = struct.pack("<IH", len(data), 4096) + bad + nc4.str_u16("bad")
    link.request(0x2201, payload, expect={0x2204}, timeout=2)
    link.request(0x2202, struct.pack("<I", 0) + data, expect={0x2204}, timeout=2)
    cmd, rsp = link.request(0x2203, b"", expect={0x221B, 0x221C}, timeout=2)
    assert cmd == 0x221C
    err = ota._parse_error(rsp)
    assert "err_end_validate_fail" in err.names
