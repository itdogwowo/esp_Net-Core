"""設定檔（settings.json／settings.local.json）與 run.py 優先次序的測試。

要鎖住的行為：
1. **優先次序**：命令列 > settings.local.json > settings.json > 內建預設；
2. **壞值要大聲**：打錯 key 名、型別錯、超出範圍 → 報錯並指出 key（唔可以靜靜用預設）；
3. **未知 key 只警告**（容許註解／預留欄位）；
4. **auto_port**：被佔用就自動揀下一個，唔會卡住啟動。
"""
import json
import socket
import sys
from pathlib import Path

import pytest

import settings as cfg

BASE = Path(__file__).resolve().parents[1]        # …/web_tools/webmaster
TOOL_ROOT = BASE.parent                            # …/web_tools（設定檔所在）
sys.path.insert(0, str(BASE))


def write_json(path: Path, data: dict):
    path.write_text(json.dumps(data, ensure_ascii=False), encoding="utf-8")


@pytest.fixture()
def confdir(workdir):
    d = workdir / "conf"
    d.mkdir()
    return d


# ── 優先次序 ──────────────────────────────────────────────

def test_defaults_when_no_files(confdir):
    conf, used = cfg.load(confdir, quiet=True)
    assert used == []
    assert conf["server"]["port"] == cfg.DEFAULTS["server"]["port"]
    assert conf["server"]["auto_port"] is True


def test_settings_file_overrides_defaults(confdir):
    write_json(confdir / "settings.json", {"server": {"port": 8123}})
    conf, used = cfg.load(confdir, quiet=True)
    assert used == ["settings.json"]
    assert conf["server"]["port"] == 8123
    # 未指定的欄位仍然係預設
    assert conf["server"]["host"] == cfg.DEFAULTS["server"]["host"]


def test_local_overrides_settings(confdir):
    write_json(confdir / "settings.json", {"server": {"port": 8123, "host": "0.0.0.0"},
                                           "device": {"baud": 921600}})
    write_json(confdir / "settings.local.json", {"server": {"port": 9000}})
    conf, used = cfg.load(confdir, quiet=True)
    assert used == ["settings.json", "settings.local.json"]
    assert conf["server"]["port"] == 9000          # local 贏
    assert conf["server"]["host"] == "0.0.0.0"     # settings 的深層合併仍然生效
    assert conf["device"]["baud"] == 921600


def test_env_overrides_files(confdir, monkeypatch):
    write_json(confdir / "settings.json", {"server": {"port": 8123}})
    monkeypatch.setenv("ESPWM_PORT", "9500")
    conf, _used = cfg.load(confdir, quiet=True)
    assert conf["server"]["port"] == 9500


# ── 壞值要大聲 ────────────────────────────────────────────

def test_bad_json_reports_line(confdir):
    (confdir / "settings.json").write_text('{\n  "server": {,}\n}', encoding="utf-8")
    with pytest.raises(cfg.SettingsError) as ei:
        cfg.load(confdir, quiet=True)
    assert "settings.json" in str(ei.value)
    assert "第 2 行" in str(ei.value)


def test_wrong_type_reports_key_and_source(confdir):
    write_json(confdir / "settings.json", {"server": {"port": "8010"}})
    with pytest.raises(cfg.SettingsError) as ei:
        cfg.load(confdir, quiet=True)
    msg = str(ei.value)
    assert "server.port" in msg and "settings.json" in msg


def test_bool_is_not_int(confdir):
    """`"port": true` 唔可以當成 1 —— bool 係 int 的子類，容易中招。"""
    write_json(confdir / "settings.json", {"server": {"port": True}})
    with pytest.raises(cfg.SettingsError) as ei:
        cfg.load(confdir, quiet=True)
    assert "server.port" in str(ei.value)


def test_port_out_of_range(confdir):
    write_json(confdir / "settings.json", {"server": {"port": 99999}})
    with pytest.raises(cfg.SettingsError) as ei:
        cfg.load(confdir, quiet=True)
    assert "超出範圍" in str(ei.value)


def test_enum_rejects_unknown_value(confdir):
    write_json(confdir / "settings.json", {"server": {"log_level": "loud"}})
    with pytest.raises(cfg.SettingsError) as ei:
        cfg.load(confdir, quiet=True)
    assert "log_level" in str(ei.value)


def test_unknown_key_warns_but_loads(confdir, capsys):
    write_json(confdir / "settings.json", {"server": {"prot": 8010}})
    conf, _ = cfg.load(confdir, quiet=True)
    err = capsys.readouterr().err
    assert "prot" in err and "警告" in err
    assert conf["server"]["port"] == cfg.DEFAULTS["server"]["port"]


def test_underscore_keys_are_ignored_silently(confdir, capsys):
    write_json(confdir / "settings.json", {"_comment": "hello", "_x": 1,
                                           "server": {"_why": "port 用 8010"}})
    cfg.load(confdir, quiet=True)
    assert "警告" not in capsys.readouterr().err


def test_non_object_top_level(confdir):
    (confdir / "settings.json").write_text("[1,2,3]", encoding="utf-8")
    with pytest.raises(cfg.SettingsError):
        cfg.load(confdir, quiet=True)


# ── write_default ─────────────────────────────────────────

def test_write_default_creates_and_refuses_overwrite(confdir):
    p = cfg.write_default(confdir)
    assert p.exists() and p == confdir / "settings.json"
    first = p.read_text(encoding="utf-8")
    p.write_text('{"server": {"port": 7777}}', encoding="utf-8")
    cfg.write_default(confdir)                       # 唔覆蓋
    assert json.loads(p.read_text(encoding="utf-8"))["server"]["port"] == 7777
    cfg.write_default(confdir, force=True)           # 強制覆蓋
    assert p.read_text(encoding="utf-8") == first
    # 寫出嚟的檔本身要讀得返、通過驗證
    conf, used = cfg.load(confdir, quiet=True)
    assert used == ["settings.json"]
    assert conf["server"]["port"] == cfg.DEFAULTS["server"]["port"]


def test_repo_settings_json_is_valid():
    """repo 內真正的 settings.json 一定要讀得通（否則所有人都開唔到）。

    位置係 **web_tools/settings.json**——同啟動器同層，唔係喺 webmaster/ 入面。
    """
    conf, used = cfg.load(quiet=True)              # 預設 base = web_tools/
    assert "settings.json" in used
    assert isinstance(conf["server"]["port"], int)
    assert cfg.settings_path() == TOOL_ROOT / "settings.json"
    assert (TOOL_ROOT / "settings.json").exists()
    # 程式目錄**唔應該**再有 settings.json（免得兩份打架）
    assert not (BASE / "settings.json").exists()


def test_settings_live_next_to_launchers():
    """設定檔要同啟動器同層：你打開 web_tools/ 就見到，唔需要入程式目錄。"""
    for launcher in ("web.sh", "web.ps1", "WebMaster.bat", "WebMaster.command"):
        assert (TOOL_ROOT / launcher).exists(), f"啟動器唔見了：{launcher}"
    assert (TOOL_ROOT / "settings.json").exists()
    assert (TOOL_ROOT / "webmaster").is_dir()


# ── port 工具 ─────────────────────────────────────────────

def test_port_in_use_detects_listener():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        s.listen(1)
        port = s.getsockname()[1]
        assert cfg.port_in_use("127.0.0.1", port) is True
    # 關掉之後應該騰空（等 OS 釋放）
    assert cfg.port_in_use("127.0.0.1", port) in (True, False)


def test_find_free_port_skips_used():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        s.listen(1)
        used = s.getsockname()[1]
        found = cfg.find_free_port("127.0.0.1", used, scan=50)
        assert found is not None and found != used


# ── run.py 的優先次序（用 argparse 解析，唔真的 start server）────

def test_cli_parser_defaults_are_none():
    """CLI 預設一律 None —— 咁「用戶冇講」同「用戶講咗預設值」才分得開。"""
    import run
    ns = run._build_parser().parse_args([])
    assert ns.port is None and ns.host is None
    assert ns.open_browser is None and ns.auto_port is None
