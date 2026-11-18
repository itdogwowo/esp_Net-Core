"""`/api/format` 同 `formatting.py` 的測試。

格式化係「server 係唯一真相」：UI 按格式化 → 打呢個 API → 改編輯器內容。
所以三件事要鎖住：正確輸出、壞 JSON 唔改內容（帶行／欄）、非 JSON 拒絕。
"""
import json

import formatting
import pytest
from fastapi.testclient import TestClient

from server import create_app


def test_format_json_basic():
    r = formatting.format_text("/config/system.json", '{"b":2,"a":1}')
    assert r["changed"] is True
    assert r["text"] == '{\n  "b": 2,\n  "a": 1\n}\n'
    assert r["bytes_out"] > r["bytes_in"]


def test_format_json_already_standard_is_noop():
    src = '{\n  "a": 1\n}\n'
    r = formatting.format_text("/config/x.json", src)
    assert r["changed"] is False
    assert r["text"] == src


def test_format_json_sort_keys():
    r = formatting.format_text("/config/x.json", '{"b":2,"a":1}', sort_keys=True)
    assert r["text"].index('"a"') < r["text"].index('"b"')


def test_format_json_utf8_not_escaped():
    r = formatting.format_text("/config/x.json", '{"名":"值"}')
    assert "名" in r["text"] and "\\u" not in r["text"]


def test_format_rejects_bad_json_with_position():
    with pytest.raises(formatting.FormatError) as ei:
        formatting.format_text("/config/x.json", '{\n  "a": }')
    err = ei.value
    assert err.line == 2
    assert err.col >= 1
    assert str(err)


def test_format_rejects_non_json_path():
    with pytest.raises(formatting.FormatError):
        formatting.format_text("/config/x.txt", "hello")


def test_format_rejects_oversized():
    big = '{"a":"' + "x" * formatting.MAX_INPUT + '"}'
    with pytest.raises(formatting.FormatError):
        formatting.format_text("/config/x.json", big)


def test_format_api_ok(workdir):
    app = create_app(sim_root=workdir / "fs")
    with TestClient(app) as c:
        r = c.post("/api/format", json={"path": "/config/x.json",
                                        "text": '{"a":1}'})
        assert r.status_code == 200, r.text
        body = r.json()
        assert body["ok"] is True
        assert body["data"]["text"] == '{\n  "a": 1\n}\n'


def test_format_api_bad_json_is_200_with_detail(workdir):
    app = create_app(sim_root=workdir / "fs2")
    with TestClient(app) as c:
        r = c.post("/api/format", json={"path": "/config/x.json", "text": '{"a":}'})
        assert r.status_code == 200
        body = r.json()
        assert body["ok"] is False
        assert body["detail"]["line"] == 1
        # 唔可以回半成品文字：UI 一改就毀資料
        assert "text" not in body


def test_format_api_requires_text(workdir):
    app = create_app(sim_root=workdir / "fs3")
    with TestClient(app) as c:
        assert c.post("/api/format", json={"path": "/config/x.json"}).status_code == 400
