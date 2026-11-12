import hashlib
import json
import struct

import pytest

import files
import nc4
from sim import SimDevice
from link import SimLink


@pytest.fixture()
def link(workdir):
    dev = SimDevice(workdir / "fs")
    return SimLink(dev)


def test_manifest_scan(link):
    files.scan(link)
    m = files.fetch_manifest(link)
    assert "/config/system.json" in m
    ent = m["/config/system.json"]
    assert ent["s"] > 0
    assert len(ent["h"]) == 64
    assert "/manifest.json" not in m


def test_upload_first_time_no_pending(link):
    q = files.upload(link, "/config/hello.json", b'{"x":1}')
    assert q["exists"] is True
    assert q["size"] == 7
    assert q["pending"] is False
    assert files.download(link, "/config/hello.json") == b'{"x":1}'


def test_overwrite_then_confirm(link):
    files.upload(link, "/config/hello.json", b"v1")
    q = files.upload(link, "/config/hello.json", b"v2")
    assert q["pending"] is True
    assert files.download(link, "/config/hello.json") == b"v2"
    q2 = files.confirm(link, "/config/hello.json")
    assert q2["pending"] is False
    assert files.download(link, "/config/hello.json") == b"v2"


def test_undo_restores_bak(link):
    files.upload(link, "/config/hello.json", b"old")
    files.upload(link, "/config/hello.json", b"new")
    q = files.undo(link, "/config/hello.json")
    assert q["exists"] is True
    assert q["pending"] is False
    assert files.download(link, "/config/hello.json") == b"old"


def test_delete(link):
    files.upload(link, "/config/tmp.txt", b"bye")
    q = files.delete(link, "/config/tmp.txt")
    assert q["exists"] is False
    with pytest.raises(files.FileError):
        files.download(link, "/config/tmp.txt")


def test_large_upload_chunking(link):
    data = bytes(range(256)) * 40          # 10 KiB
    files.upload(link, "/pixel/big.bin", data, chunk_size=1024)
    assert files.download(link, "/pixel/big.bin", chunk_size=1024) == data


def test_busy_when_fat_not_owned(link):
    # file_mode=0 → usb 模式；寫入要 busy
    link.request(0x1103, nc4.str_u16('{"System":{"file_mode":0}}'),
                 expect={0x1104}, timeout=2)
    with pytest.raises(files.FileError) as e:
        files.upload(link, "/config/x.json", b"1")
    assert e.value.kind == "err_busy"
    link.request(0x1103, nc4.str_u16('{"System":{"file_mode":1}}'),
                 expect={0x1104}, timeout=2)


def test_sha_mismatch(link):
    data = b"abc"
    wrong = b"\x00" * 32
    payload = struct.pack("<HIH", 1, len(data), 4096) + wrong + nc4.str_u16("/config/bad.txt")
    link.request(0x2001, payload, expect={0x2004, 0x2010}, timeout=2)
    link.request(0x2002, struct.pack("<HI", 1, 0) + data,
                 expect={0x2004, 0x2010}, timeout=2)
    cmd, rsp = link.request(0x2003, struct.pack("<H", 1),
                            expect={0x2006, 0x2010}, timeout=2)
    assert cmd == 0x2010
    assert files.parse_error(rsp).kind == "err_sha_mismatch"


def test_offset_mismatch(link):
    payload = struct.pack("<HIH", 1, 4, 4096) + hashlib.sha256(b"abcd").digest() + nc4.str_u16("/config/o.txt")
    link.request(0x2001, payload, expect={0x2004, 0x2010}, timeout=2)
    cmd, rsp = link.request(0x2002, struct.pack("<HI", 1, 2) + b"cd",
                            expect={0x2004, 0x2010}, timeout=2)
    assert cmd == 0x2010
    assert files.parse_error(rsp).kind == "err_offset_mismatch"


def test_manifest_marks_pending(link):
    files.upload(link, "/config/p.json", b"1")
    files.upload(link, "/config/p.json", b"2")
    files.scan(link)
    m = files.fetch_manifest(link)
    assert m["/config/p.json"]["p"] == 1
    files.confirm(link, "/config/p.json")
    m = files.fetch_manifest(link)
    assert m["/config/p.json"]["p"] == 0
