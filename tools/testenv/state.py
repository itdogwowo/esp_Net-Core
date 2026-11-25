"""本機狀態：state dir、machine.json、last-probe.json。

為甚麼唔放 git：
  · 每台機的 IDF／板／image 都唔同，入 git 只會互相覆蓋，變成「最後一次 commit
    的人」的環境；
  · 這些是現場事實，不是專案契約。契約在 tools/testenv/policy.json（進 git）。

預設位置：`<repo>/.testenv/`（已 gitignore）。如果整個 repo 放在 iCloud／Dropbox，
那個資料夾一樣會被同步；用 `--state-dir` 或 `ESP_NET_CORE_TESTENV_DIR` 指去
`~/.esp_net_core/testenv`。
"""
from __future__ import annotations

import hashlib
import json
import os
import platform
import re
import subprocess
import sys
import uuid
from datetime import datetime
from pathlib import Path

from . import TenvError

SCHEMA = 1
STATE_ENV = "ESP_NET_CORE_TESTENV_DIR"
DEFAULT_DIRNAME = ".testenv"

# 路徑上見到這些字眼 = 這個 repo 在雲端同步夾裡面，.testenv 會跟住走。
SYNC_MARKERS = (
    "Library/Mobile Documents", "Dropbox", "OneDrive", "Google Drive", "GoogleDrive",
    "iCloud", "pCloud", "MEGAsync", "Nextcloud", "Syncthing",
)


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def now_iso() -> str:
    return datetime.now().astimezone().isoformat(timespec="seconds")


def resolve_state_dir(repo: Path | None = None, explicit: str | None = None) -> Path:
    """state dir 的解析順序：命令列 > 環境變數 > <repo>/.testenv。"""
    repo = repo or repo_root()
    raw = explicit or os.environ.get(STATE_ENV) or ""
    if raw.strip():
        return Path(raw).expanduser().resolve()
    return (repo / DEFAULT_DIRNAME).resolve()


def _run(args: list[str], timeout: float = 5.0) -> tuple[int, str]:
    try:
        r = subprocess.run(args, capture_output=True, text=True, encoding="utf-8",
                           errors="replace", timeout=timeout)
        return r.returncode, (r.stdout or "") + (r.stderr or "")
    except (OSError, subprocess.TimeoutExpired):
        return 1, ""


def _hardware_key() -> str | None:
    """機身指紋的原始字串（唔同 OS 用唔同來源）。

    ⚠ 唔可以用 uuid.getnode() 做唯一來源：Python 拿不到 MAC 時會**每次隨機**生成
    一個（multicast bit = 1）。2026-11-26 在 macOS 實測：同一個 process 內穩定，
    但每次跑 tenv 都變 → 每個命令都說「這是另一台機」。這正是「自動識別」最不
    應該出的錯，所以 macOS 直接用 IOPlatformUUID。
    """
    if sys.platform == "darwin":
        rc, out = _run(["ioreg", "-rd1", "-c", "IOPlatformExpertDevice"], timeout=5)
        m = re.search(r'"IOPlatformUUID"\s*=\s*"([^"]+)"', out) if rc == 0 else None
        if m:
            return "ioreg:" + m.group(1)
    for p in ("/etc/machine-id", "/var/lib/dbus/machine-id"):
        try:
            text = Path(p).read_text(encoding="utf-8", errors="ignore").strip()
        except OSError:
            continue
        if text:
            return f"machine-id:{text}"
    if os.name == "nt":
        rc, out = _run(["reg", "query", r"HKLM\SOFTWARE\Microsoft\Cryptography",
                        "/v", "MachineGuid"], timeout=8)
        m = re.search(r"MachineGuid\s+REG_SZ\s+(\S+)", out) if rc == 0 else None
        if m:
            return "machineguid:" + m.group(1)
    node = uuid.getnode()
    # multicast bit = 1 → 這是 Python 隨機生成的假 MAC，唔可以當機身指紋。
    if not (node >> 40) & 1:
        return f"mac:{node:012x}"
    return None


def node_id() -> str:
    """穩定的機器指紋（12 hex）。拿不到硬件 ID 就退回 hostname 組合。"""
    key = _hardware_key()
    if not key:
        key = f"host:{platform.node()}|{platform.system()}|{platform.machine()}"
    return hashlib.sha256(key.encode("utf-8")).hexdigest()[:12]


def make_machine_id(host: dict) -> str:
    os_name = re.sub(r"[^a-z0-9]+", "-", str(host.get("os") or "unknown").lower()).strip("-")
    arch = re.sub(r"[^a-z0-9]+", "-", str(host.get("arch") or "unknown").lower()).strip("-")
    return f"{os_name}-{arch}-{node_id()}"


def default_label(host: dict) -> str:
    hostname = str(host.get("hostname") or "").split(".")[0].strip().lower()
    hostname = re.sub(r"[^a-z0-9_-]+", "-", hostname).strip("-") or "machine"
    return f"{host.get('os', '?')}-{host.get('arch', '?')}-{hostname}"


def default_machine(host: dict, label: str | None = None) -> dict:
    now = now_iso()
    return {
        "schema": SCHEMA,
        "created_at": now,
        "updated_at": now,
        "machine": {
            "id": make_machine_id(host),
            "label": label or default_label(host),
            "role": "dev",
            "hostname": host.get("hostname") or "",
            "os": host.get("os") or "",
            "arch": host.get("arch") or "",
        },
        "idf": {"pin": None},
        "board": {
            "expected": None,
            "expected_cid": None,
            "project": None,
            "absent_ok": False,
        },
        "lanes": {"disabled": []},
        "notes": "",
    }


def _merge_defaults(m: dict, base: dict) -> dict:
    """舊版 machine.json 缺欄位時補預設；已有的值一律保留（向前兼容）。"""
    out = dict(base)
    for k, v in m.items():
        if isinstance(v, dict) and isinstance(out.get(k), dict):
            merged = dict(out[k])
            merged.update(v)
            out[k] = merged
        else:
            out[k] = v
    return out


def ensure_state_dir(d: Path) -> None:
    d.mkdir(parents=True, exist_ok=True)
    try:
        os.chmod(d, 0o700)      # 紀錄含主機路徑；唔需要給同機其他用戶讀
    except OSError:
        pass


def _atomic_write_json(path: Path, data: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    os.replace(tmp, path)


def machine_path(d: Path) -> Path:
    return d / "machine.json"


def probe_path(d: Path) -> Path:
    return d / "last-probe.json"


def load_machine(d: Path) -> dict:
    p = machine_path(d)
    if not p.exists():
        raise TenvError(
            f"這台機還沒有測試環境紀錄：{p}\n"
            f"  → python3 tools/tenv.py init"
        )
    try:
        raw = json.loads(p.read_text(encoding="utf-8"))
    except json.JSONDecodeError as e:
        raise TenvError(f"{p} 不是合法 JSON：{e}")
    if not isinstance(raw, dict):
        raise TenvError(f"{p} 的最外層必須是物件")
    host = {
        "os": (raw.get("machine") or {}).get("os", ""),
        "arch": (raw.get("machine") or {}).get("arch", ""),
        "hostname": (raw.get("machine") or {}).get("hostname", ""),
    }
    return _merge_defaults(raw, default_machine(host))


def save_machine(d: Path, m: dict) -> None:
    m["schema"] = SCHEMA
    m["updated_at"] = now_iso()
    _atomic_write_json(machine_path(d), m)


def load_probe(d: Path) -> dict | None:
    p = probe_path(d)
    if not p.exists():
        return None
    try:
        raw = json.loads(p.read_text(encoding="utf-8"))
        return raw if isinstance(raw, dict) else None
    except (json.JSONDecodeError, OSError):
        return None


def save_probe(d: Path, facts: dict) -> None:
    _atomic_write_json(probe_path(d), facts)


def sync_risk(repo: Path) -> str | None:
    s = str(repo).replace("\\", "/")
    for marker in SYNC_MARKERS:
        if marker.lower() in s.lower():
            return marker
    return None


def is_gitignored(repo: Path, path: Path) -> bool | None:
    """`git check-ignore` 的結果；唔係 git repo／冇 git 就回 None。"""
    try:
        r = subprocess.run(
            ["git", "-C", str(repo), "check-ignore", "-q", str(path)],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5,
        )
        return r.returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return None


def state_readme(repo: Path) -> str:
    return f"""# 這台機的測試環境紀錄（tenv）

> 這個資料夾係**本機專用**，唔會經 git 同步，亦唔應該 commit。
> 換電腦＝另一份；clone 完 repo 跑一次 `python3 tools/tenv.py init`。

| 檔案 | 誰寫 | 內容 |
|---|---|---|
| `machine.json` | 你＋`tenv set` | 這台機的**意圖**：常接邊塊板、用邊個 IDF pin、邊條 lane 關掉 |
| `last-probe.json` | `tenv plan` 自動 | 上一次**事實**：IDF 版本、序列埠、板、板上 image 能力 |

## 常用

```bash
python3 tools/tenv.py brief      # 每次開工先睇這個
python3 tools/tenv.py plan -v    # 邊條 lane 跑得、閘門係咩
python3 tools/tenv.py stamp      # 貼入測試紀錄的一行環境戳記
python3 tools/tenv.py verify     # 紀錄同現實有冇漂移（唔會寫檔）
```

## 規矩

- `blocked`／`unknown` **唔等於測過**；唔可以把 host 數字外推成板上的數字。
- 回報任何數字前貼 `tenv stamp`；跨 IDF 版本的數字唔可以互相對比。
- 唔好手改 `last-probe.json`（下次 plan 會覆蓋）；要改意圖改 `machine.json`。

repo：`{repo}`
"""
