import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import container                      # noqa: E402
from _ncm_sample import make_container  # noqa: E402

REPO = Path(__file__).resolve().parents[3]   # web_tools/webmaster/tests → repo 根


def test_valid_task_container():
    blob = make_container(cmds=(0x3101, 0x3102))
    c = container.parse_container(blob, "/tasks/demo.nct")
    assert c["valid"] is True, c["errors"]
    assert c["file"]["ext_known"] is True
    assert c["header"]["magic"] == "0x314D434E"
    assert c["manifest"]["kind_name"] == "task"
    assert c["manifest"]["core"] == -1
    assert c["manifest"]["ncmds"] == 2
    assert c["manifest"]["cmds"] == [0x3101, 0x3102]
    assert c["meta"]["name"] == "test-mod"
    assert c["meta"]["requires"][0]["name"] == "gpio"
    assert c["meta"]["requires"][0]["exclusive"] is True
    assert c["meta"]["provides"][0]["name"] == "test.heartbeat"
    assert c["payload"]["crc_ok"] is True


def test_bad_magic_not_a_container():
    c = container.parse_container(b"hello world" * 8, "notes.txt")
    assert c["valid"] is False
    assert any("magic" in e for e in c["errors"])


def test_truncated_and_bad_crc():
    blob = make_container()
    assert container.parse_container(blob[:30], "x.nct")["valid"] is False
    tampered = bytearray(blob)
    tampered[-1] ^= 0xFF
    c = container.parse_container(bytes(tampered), "x.nct")
    assert c["valid"] is False
    assert c["payload"]["crc_ok"] is False


def test_nca_extension_warning():
    c = container.parse_container(make_container(), "/tasks/thing.nca")
    assert c["file"]["ext_known"] is False
    assert any("nct" in w for w in c["warnings"])


def test_real_sample_if_built():
    sample = REPO / "modules" / "out" / "test_hello_tasks.nct"
    if not sample.exists():
        return
    c = container.parse_container(sample.read_bytes(), sample.name)
    assert c["valid"] is True, c["errors"]
    assert c["meta"]["name"].startswith("test-hello")
