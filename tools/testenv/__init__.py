"""testenv —— 每台機的測試環境紀錄（tenv）。

三層分工（詳見 docs/02_guides/test-environment.md）：

    policy.json   團隊契約（進 git）：IDF 要幾多、幾支 host test。全部機器共用。
    .testenv/     本機紀錄（唔進 git）：這台機的意圖（常接邊塊板）＋上次探測事實。
    probe         每次開工重探：IDF／序列埠／板／板上 image 能力。事實永遠贏過記憶。

設計原則：**唔信任手寫紀錄**。machine.json 只記「這台機想用甚麼」，實際能不能跑由
probe 回答；plan 把兩者對照後才講「邊條 lane 跑得」。所以紀錄過期不會令 AI 以為
自己測過——只會令 plan 顯示 drift，叫你再探一次。
"""
from __future__ import annotations

import json
import re
from pathlib import Path

__version__ = "1.0"

DEFAULT_POLICY_PATH = Path(__file__).with_name("policy.json")


class TenvError(RuntimeError):
    """可以預期、要印給用戶看的錯誤（唔使 traceback）。"""


def load_policy(path: Path | None = None) -> dict:
    p = path or DEFAULT_POLICY_PATH
    try:
        data = json.loads(p.read_text(encoding="utf-8"))
    except FileNotFoundError:
        raise TenvError(f"找不到政策檔：{p}")
    except json.JSONDecodeError as e:
        raise TenvError(f"{p} 不是合法 JSON：{e}")
    if not isinstance(data, dict):
        raise TenvError(f"{p} 的最外層必須是物件")
    data.setdefault("schema", 1)
    data.setdefault("idf", {})
    data.setdefault("host_tests", {})
    data.setdefault("probe", {})
    return data


_VER_RE = re.compile(r"(\d+)\.(\d+)(?:\.(\d+))?")


def version_tuple(v: str | None) -> tuple[int, int, int] | None:
    """把 `v6.1.1`／`6.1`／`v5.5.5-...` 拆成 (major, minor, patch)。

    缺 patch 當 0，所以 `v6.1` != `v6.1.1`——這正是我們要分得出來的差異。
    讀不到數字回 None（唔好當成 0，否則「版本未知」會靜靜變成「同版」）。
    """
    if not v:
        return None
    m = _VER_RE.search(str(v))
    if not m:
        return None
    return (int(m.group(1)), int(m.group(2)), int(m.group(3) or 0))


def version_matches(found: str | None, required: str | None, mode: str = "exact") -> bool | None:
    """found 是否符合 required。讀不到任何一邊回 None（unknown，不是 False）。"""
    a, b = version_tuple(found), version_tuple(required)
    if a is None or b is None:
        return None
    if mode == "minor":
        return a[:2] == b[:2]
    return a == b


def human_age(seconds: float) -> str:
    s = int(max(0, seconds))
    if s < 90:
        return f"{s} 秒"
    if s < 5400:
        return f"{s // 60} 分鐘"
    if s < 172800:
        return f"{s // 3600} 小時"
    return f"{s // 86400} 日"
