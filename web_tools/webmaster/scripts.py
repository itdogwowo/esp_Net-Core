#!/usr/bin/env python3
"""一鍵啟動腳本：分類、偵測、擷取。

WebMaster 係「人用嘅入口」，所以 repo 根嘅啟動腳本要擺喺 UI 最前面（首頁），
而且要講清楚「邊個平台用邊個檔」。呢個模組係唯一真相：

- **平台**：`windows` / `macos` / `linux` / `any`。
- **分類**（固定次序，UI 照跟）：
  `webmaster`（網頁工作臺）→ `build`（建置／燒錄）→ `device`（板子／模擬器）→ `ai`（AI／MCP）。
- **偵測**：主要靠副檔名（`.ps1`／`.bat`／`.cmd` → Windows；`.command` → macOS；
  `.sh` → any），再用檔名關鍵字細分（`web`／`flash`／`build`／`sim`…）。
- **擷取**：送出前要防路徑穿越——只准讀 repo 根底下嘅已知腳本副檔名；
  唔跟 symlink，唔許回原始位元組（腳本係文字，UTF-8 解碼）。

呢度唔會執行任何腳本：WebMaster 只負責「話你知用邊個、點用、內容係咩」。
"""
from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path

# 平台（次序＝UI 顯示次序）
PLATFORMS = ("windows", "macos", "linux", "any")

# 分類：key → (標題, 一句說明, 圖示)
CATEGORIES: list[tuple[str, str, str, str]] = [
    ("webmaster", "網頁工作臺", "開 WebMaster：自動建 venv、裝 deps、開瀏覽器", "🖥"),
    ("build", "建置／燒錄", "編譯固件、上傳映像、環境健檢", "🔨"),
    ("device", "板子／模擬器", "裝置模擬、序列埠、測試", "🔌"),
    ("ai", "AI／整合", "AI 助手、MCP、其他自動化入口", "🤖"),
]

# 副檔名 → 平台（一個副檔名可以對多個平台：`sh` 同時係 macOS／Linux）
EXT_PLATFORMS: dict[str, tuple[str, ...]] = {
    ".ps1": ("windows",),
    ".bat": ("windows",),
    ".cmd": ("windows",),
    ".command": ("macos",),
    ".sh": ("macos", "linux"),
    ".py": ("any",),
}

# 只准擷取呢啲副檔名（避免變成任意檔案讀取）
TEXT_EXTS = set(EXT_PLATFORMS)

# 檔名關鍵字 → 分類（先命中先算；次序＝優先）
# 注意：`build` 喺 webmaster 之後——`web.sh`／`run_web.sh` 唔會俾 `build` 搶走。
CATEGORY_RULES: list[tuple[str, tuple[str, ...]]] = [
    ("webmaster", ("web", "webmaster")),
    ("build", ("build", "flash", "make", "ota", "release")),
    ("ai", ("ai", "mcp", "agent", "llm", "copilot")),
    ("device", ("sim", "device", "serial", "monitor", "test", "bench", "probe")),
]

# 唔係「人用啟動腳本」嘅目錄／檔名（工具內部、測試產物、依賴）
SKIP_DIRS = {
    ".git", ".venv", "venv", "node_modules", "__pycache__", "build", "dist",
    "tools", "firmware", "modules", "ports", "artifacts", "docs", "webmaster",
    "tests", "test", ".pytest_cache", ".mypy_cache", ".ruff_cache", ".idea",
}
SKIP_NAMES = {
    "make.py", "setup.py", "conftest.py", "run.py", "launch.py", "server.py",
    "__init__.py", "__main__.py",
}
SKIP_PREFIX = ("_", ".")  # 內部腳本（_helper.sh、.local.sh）唔當入口

# 檔頭「前奏指令」：喺真正註解區之前出現，唔算「第一個真指令」。
# 唔跳過嘅話，`@echo off`（.bat 第一行）會令整段 REM 用法說明被當成程式碼。
_PREAMBLE = (
    "@echo", "echo off", "setlocal", "endlocal", "rem ", "::",
    "#!/", "set -e", "set -u", "set -o", "set -a",
    "use strict", "from __future__", "import ", "shopt ",
)


def _is_preamble(s: str) -> bool:
    low = s.lower()
    return any(low.startswith(p) for p in _PREAMBLE)

# 已知入口腳本：中文摘要（優先於檔頭註解）。
# 理由：.bat 係純 ASCII（cmd 代碼頁問題），檔頭只能寫英文；UI 係中文，
# 所以喺呢度補一句人話。冇列出嘅腳本照樣會用檔頭註解，唔會漏。
# 路徑一律由 **repo 根** 起算（2026-11-18 起全部網頁工具收喺 web_tools/）。
KNOWN: dict[str, dict[str, str]] = {
    "WebMaster.bat": {
        "summary": "Windows 雙擊即開（預設模擬裝置，免硬件）",
        "hint": "雙擊；或喺 repo 根打：.\\web_tools\\WebMaster.bat --serial COM13",
    },
    "web.ps1": {
        "summary": "Windows 完整啟動器：揀 Python、建 venv、裝 deps、檢查 port",
        "hint": "repo 根：.\\web_tools\\web.ps1          真板：… -Serial COM13",
    },
    "web.sh": {
        "summary": "macOS／Linux 一鍵開（自動 venv＋開瀏覽器）",
        "hint": "repo 根：./web_tools/web.sh          模擬：… --sim",
    },
    "run_web.sh": {
        "summary": "Linux 通用入口（內容同 web.sh，穩妥處理 cd）",
        "hint": "./web_tools/run_web.sh",
    },
    "WebMaster.command": {
        "summary": "macOS 雙擊即開（Terminal 停住睇 log）",
        "hint": "Finder 雙擊 web_tools／WebMaster.command",
    },
    "make.bat": {
        "summary": "建置／燒錄／健檢（= python make.py）",
        "hint": "make.bat doctor ｜ make.bat build s3 ｜ make.bat build --all",
    },
    "build.sh": {
        "summary": "建置／燒錄／健檢（= python3 make.py）",
        "hint": "./build.sh doctor ｜ ./build.sh build s3 ｜ ./build.sh build --all",
    },
}

# 用嚟估「呢個腳本做咩」：抽前 80 行、最多 40 條註解
_COMMENT_LIMIT = 40
_PREVIEW_LINES = 80


@dataclass
class Script:
    name: str
    rel: str                 # 相對 repo 根（`web_tools/web.sh` 或 `make.bat`）
    location: str            # `web_tools` 或 `.`（邊一層）
    platform: str
    platforms: list[str]
    ext: str
    category: str
    summary: str
    hint: str
    runnable: bool
    args: list[str] = field(default_factory=list)
    size: int = 0
    updated: float = 0.0

    def as_dict(self) -> dict:
        return {
            "name": self.name,
            "rel": self.rel,
            "location": self.location,
            "platform": self.platform,
            "platforms": self.platforms,
            "ext": self.ext,
            "category": self.category,
            "summary": self.summary,
            "hint": self.hint,
            "runnable": self.runnable,
            "args": self.args,
            "size": self.size,
            "updated": self.updated,
        }


def ext_of(name: str) -> str:
    """`WebMaster.command` → `.command`；`web.sh` → `.sh`；冇副檔名 → `""`。"""
    lower = name.lower()
    for ext in EXT_PLATFORMS:
        if lower.endswith(ext):
            return ext
    return ""


def platform_of(name: str) -> str | None:
    ext = ext_of(name)
    if not ext:
        return None
    plats = EXT_PLATFORMS[ext]
    # `.sh` 同時係 macOS／Linux：用 any 之外嘅第一順位（macOS）做代表，
    # `hint` 會另外列出兩個平台。
    return "macos" if ext == ".sh" else plats[0]


def category_of(name: str, rel: str) -> str:
    hay = f"{name} {rel}".lower()
    for cat, words in CATEGORY_RULES:
        if any(w in hay for w in words):
            return cat
    return "device"


def _clean(line: str) -> str:
    s = line.strip()
    s = re.sub(r"^<#|#>$", "", s).strip()
    s = re.sub(r"^(#|//|::|REM\b|--|;)\s*", "", s, flags=re.I)
    return s.strip()


def _is_comment(s: str) -> bool:
    return (s.startswith(("#", "::", "--", ";", "<#"))
            or s.upper().startswith("REM ") or s == "REM" or s.startswith("//"))


def _header_comments(text: str) -> list[str]:
    """抽檔頭註解：由第一行開始嘅連續註解區（最多 80 行）。

    - 跳過 shebang（`#!/usr/bin/env`）——佢唔係說明，而且 `_clean` 之後會剩
      `!/usr/bin/env`，放入摘要會好醜；
    - 跳過成行 `====`／`----`／`════` 嘅分隔線（檔頭常用佢畫框）；
    - PowerShell／cmd 嘅塊註解（`<# … #>`、`.bat` 開頭嘅 `REM ====` 第 0 行）：
      打開之後收到 `#>` 為止——中間就算唔係 `#` 開頭都當註解；
    - 其他情況遇到第一個真指令就停。
    """
    out: list[str] = []
    in_block = False
    for raw in text.splitlines()[:_PREVIEW_LINES]:
        s = raw.strip()
        if not s:
            if out:
                out.append("")
            continue
        if s.startswith("#!"):
            continue
        if in_block:
            c = _clean(s)
            if c.startswith("#>") or c == "#>":
                in_block = False
                continue
            if c and not (set(c) <= set("=-*_# ═─━ ")):
                out.append(c.replace("#>", "").strip())
            if s.endswith("#>") and not s.startswith("#>"):
                in_block = False
            continue
        if s.startswith("<#"):
            in_block = True
            rest = _clean(s)
            if rest:
                out.append(rest)
            continue
        if not _is_comment(s):
            if out:
                break
            if _is_preamble(s):
                continue   # `@echo off` / `setlocal` / `set -euo` 等前奏，繼續搵註解區
            # 檔頭第一句就係真指令：冇註解可以抽（用分類預設文案）
            break
        c = _clean(s)
        if not c or set(c) <= set("=-*_# ═─━ "):  # 分隔線
            continue
        out.append(c)
        if len(out) >= _COMMENT_LIMIT:
            break
    cleaned: list[str] = []
    for s in out:
        if s == "" and (not cleaned or cleaned[-1] == ""):
            continue
        cleaned.append(s)
    return cleaned


def _arg_flags(text: str, ext: str) -> list[str]:
    """抽用戶面嘅參數（唔好用全檔 regex：會掃到內部實作旗標）。

    - `.ps1`：只認 `param( … )` 區塊裡面嘅 `[switch]$X`／`[string]$X` → `-X`；
    - `.sh`／`.command`：只認註解區（用法行）嘅 `--xxx`；
    - `.bat`／`.cmd`：只認註解區嘅 `--xxx`。
    去重、保留次序、最多 10 個。
    """
    found: list[str] = []
    if ext == ".ps1":
        block = re.search(r"(?ms)^\s*param\s*\((.*?)^\s*\)", text)
        if block:
            for m in re.finditer(r"\$\{?([A-Za-z_][A-Za-z0-9_]*)", block.group(1)):
                name = m.group(1)
                flag = f"-{name}"
                if flag not in found:
                    found.append(flag)
    else:
        for line in _header_comments(text):
            for m in re.finditer(r"(?<![\w-])(--[a-z][a-z0-9-]{0,24})", line):
                flag = m.group(1)
                if flag not in found:
                    found.append(flag)
    return found[:10]


def _summary(name: str, comments: list[str], category: str) -> str:
    """第一條夠長嘅註解；冇就用「分類說明」做保底（唔會回空字串）。

    如果第一條註解只係重複檔名（`web.ps1：…`），改用分類說明會好讀啲。
    """
    base = f"{name}："
    if comments:
        first = next((c for c in comments if len(c) >= 4), "")
        if first and not first.startswith(base):
            return first[:160]
    for cat, title, desc, _icon in CATEGORIES:
        if cat == category:
            return f"{title}：{desc}"
    return f"{name}：啟動腳本"


def _hint(name: str, rel: str = "") -> str:
    """跨平台命令（用相對 repo 根的路徑：文檔／截圖唔會露出本機絕對路徑）。"""
    ext = ext_of(name)
    r = rel or name
    win = r.replace("/", "\\")
    if ext == ".ps1":
        return f"PowerShell：.\\{win}（或入去雙擊）"
    if ext in (".bat", ".cmd"):
        return f"雙擊即開（cmd）；或者 .\\{win}"
    if ext == ".command":
        return "macOS 雙擊即開（Terminal 會停住睇 log）"
    if ext == ".sh":
        return f"macOS／Linux：./{r}（第一次要 chmod +x）"
    if ext == ".py":
        return f"python {r}"
    return ""


def _as_roots(roots) -> list[Path]:
    """接受單一 Path 或 Path 清單；去重、保序。"""
    if isinstance(roots, (str, Path)):
        roots = [roots]
    out: list[Path] = []
    for r in roots:
        rp = Path(r).resolve()
        if rp not in out:
            out.append(rp)
    return out


def discover(roots, base: Path | None = None) -> list[dict]:
    """掃一條或多條根（唔入子目錄）→ 啟動腳本清單。

    現時帶兩條：`web_tools/`（網頁工具）＋ repo 根（建置腳本）。
    `rel` 一律相對 `base`（預設係第一條根的 parent，即 repo 根），所以 UI 收到
    嘅路徑永遠係「由 repo 根起算」，可以直接貼落終端。
    重複檔名以先出現嘅為準。

    唔會 raise：權限問題逐個檔案跳過（Windows 上被 ACL 鎖住的目錄唔會爆）。
    """
    roots = _as_roots(roots)
    if not roots:
        return []
    base = Path(base).resolve() if base else roots[0].parent
    seen: set[str] = set()
    scripts: list[Script] = []
    for root in roots:
        try:
            children = list(root.iterdir())
        except OSError:
            continue
        for child in children:
            try:
                if child.is_symlink() or not child.is_file():
                    continue
            except OSError:
                continue
            name = child.name
            if name.startswith(SKIP_PREFIX) or name in SKIP_NAMES or name in seen:
                continue
            platform = platform_of(name)
            if platform is None:
                continue
            cat = category_of(name, name)
            try:
                st = child.stat()
                text = child.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            seen.add(name)
            comments = _header_comments(text)
            known = KNOWN.get(name, {})
            runnable = not name.lower().endswith(".py") or "__main__" in text
            try:
                rel = child.relative_to(base).as_posix()
            except ValueError:
                rel = name
            scripts.append(Script(
                name=name,
                rel=rel,
                location=root.name if root != base else ".",
                platform=platform,
                platforms=list(EXT_PLATFORMS[ext_of(name)]),
                ext=ext_of(name),
                category=cat,
                summary=known.get("summary") or _summary(name, comments, cat),
                hint=known.get("hint") or _hint(name, rel),
                runnable=runnable,
                args=_arg_flags(text, ext_of(name)),
                size=st.st_size,
                updated=st.st_mtime,
            ))
    order = {c: i for i, (c, *_rest) in enumerate(CATEGORIES)}
    scripts.sort(key=lambda s: (order.get(s.category, 99),
                                PLATFORMS.index(s.platform), s.name.lower()))
    return [s.as_dict() for s in scripts]


def _safe_target(roots: list[Path], rel: str) -> Path:
    """把相對路徑解析成真實檔案，同時擋穿越與 symlink。"""
    rel = (rel or "").replace("\\", "/").strip()
    if not rel or rel.startswith("/") or "\x00" in rel:
        raise ValueError("唔係啟動腳本")
    parts = rel.split("/")
    if any(p in ("", ".", "..") for p in parts):
        raise ValueError("唔係啟動腳本")
    if ext_of(parts[-1]) not in TEXT_EXTS:
        raise ValueError("唔係啟動腳本")
    if len(parts) > 2:
        raise ValueError("唔係啟動腳本")

    for root in roots:
        path = root.joinpath(*parts)
        if not path.exists():
            continue
        try:
            if path.is_symlink() or not path.is_file():
                raise ValueError("唔係啟動腳本")
            real = path.resolve()
            if real.parent != root and real.parent.parent != root:
                raise ValueError("唔係啟動腳本")   # 走得太深 → 拒絕
        except OSError:
            continue
        return path
    raise ValueError("搵唔到腳本")


def read(roots, rel: str, max_bytes: int = 512 * 1024) -> dict:
    """讀回一個腳本的內容（畀 UI「睇內容／複製指令」用）。

    防穿越：只接受 `<root>/<檔名>` 或 `<root>/<一層目錄>/<檔名>`、拒絕
    `.`／`..`／絕對路徑、副檔名必須喺 TEXT_EXTS 之內。唔跟 symlink。
    """
    roots = _as_roots(roots)
    path = _safe_target(roots, rel)
    try:
        st = path.stat()
        raw = path.read_bytes()[:max_bytes]
    except OSError as e:
        raise ValueError(f"讀唔到：{e}") from e
    base = roots[0].parent if path.parent in roots else path.parent
    return {
        "name": path.name,
        "rel": path.relative_to(base).as_posix(),
        "size": st.st_size,
        "truncated": st.st_size > max_bytes,
        "text": raw.decode("utf-8", errors="replace"),
    }
