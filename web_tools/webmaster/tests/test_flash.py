import flash
from fastapi.testclient import TestClient

from server import create_app


def test_build_write_cmd():
    cmd = flash.build_cmd("/dev/cu.usbmodem1401", "/tmp/app.bin", offset=0x20000,
                          baud=460800, chip="esp32p4", before="usb_reset")
    assert cmd[:3] == [flash.sys.executable, "-m", "esptool"]
    assert "--chip" in cmd and "esp32p4" in cmd
    assert "--before" in cmd and "usb_reset" in cmd
    assert "write_flash" in cmd and "0x20000" in cmd and "/tmp/app.bin" in cmd


def test_build_erase_cmd():
    cmd = flash.build_cmd("/dev/x", None, erase=True, chip="esp32s3")
    assert "erase_flash" in cmd
    assert "write_flash" not in cmd


# 用 conftest 的 `workdir` 而唔係 pytest 的 `tmp_path`：Windows sandbox 會為
# tmp_path 加一個「只俾建立者」的 ACL，之後 scandir / 建子資料夾都 WinError 5。
def test_esptool_sim_rejected(workdir):
    app = create_app(sim_root=workdir / "fs")
    with TestClient(app) as c:
        assert c.post("/api/connect", json={"sim": True}).status_code == 200
        r = c.post("/api/flash/esptool?erase=1")
        assert r.status_code == 409


def test_flash_info_sim_rejected(workdir):
    app = create_app(sim_root=workdir / "fs2")
    with TestClient(app) as c:
        assert c.post("/api/connect", json={"sim": True}).status_code == 200
        assert c.get("/api/flash/info").status_code == 409
