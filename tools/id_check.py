#!/usr/bin/env python3
"""檢查 ID token 有冇「講一個唔存在嘅東西」——漂移就會 fail。

    python tools/id_check.py [--verbose]

為什麼要有這支
-------------
`docs/03_notes/naming.md` §7 定了六個命名空間（B／T／R／L／C／M）。
規則好簡單，但規則**唔會自己執行**：改一個任務名、搬一個階段、停用一條通道，
文件裡的 `T.old_name`、`B07`、`L.ESPNOw` 就會靜靜指去空氣。之後所有人講嘢
都用一個已經唔存在嘅 token，而冇人知。

所以這支只做一件事：**文件／圖裡面出現過的 ID，喺 code 裡面要搵到對應**。

    權威來源（唯一真相）
    ─────────────────────────────────────────────
    B01…    firmware/main/main.c 的 stages[] 行（"B01" 字串）
    T.xxx   firmware 內的 xTaskCreate* 任務名 ／ nc_task_def.name
    R.xxx   nc_task_res_t.name（plan 資源）
    L.xxx   nc4_link_register / link_enabled / nc4_router_core 的字串
    C.xxxx  firmware 內出現的 0x11xx 指令字面值
    M.xxx   components/ 下的目錄名

「存在性」不等於「語義正確」：`T.decode0` 存在，但文件可能講錯它的行為。
呢個 tool 唔會、亦唔應該假裝識語義。
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DOC_GLOBS = ["docs/**/*.md", "docs/**/*.svg"]


def read(p):
    try:
        return p.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        return ""


def firmware_sources():
    out = []
    for pat in ("firmware/main/*.c", "firmware/main/*.h",
                "firmware/components/**/*.c", "firmware/components/**/*.h"):
        for p in ROOT.glob(pat):
            if "managed_components" in p.parts or "build" in p.parts:
                continue
            out.append(p)
    return out


def authority():
    """Collect the sets of names that actually exist in code."""
    auth = {"B": set(), "T": set(), "R": set(), "L": set(), "C": set(), "M": set()}

    main_c = read(ROOT / "firmware/main/main.c")
    auth["B"] = set(re.findall(r'"(B\d{2})"', main_c))

    literals = set()
    for p in firmware_sources():
        text = read(p)
        literals |= set(re.findall(r'"([A-Za-z0-9_.\-+/]{1,40})"', text))
        # 指令：0x11xx
        for h in re.findall(r"0x1[0-9A-Fa-f]{3}", text):
            auth["C"].add(h.upper().replace("0X", "0x"))
    auth["T"] = literals
    auth["R"] = literals
    auth["L"] = literals

    comp = ROOT / "firmware/components"
    if comp.is_dir():
        for p in comp.rglob("*"):
            if p.is_dir():
                auth["M"].add(p.name)
        for p in (ROOT / "firmware/main").glob("*"):
            if p.is_dir():
                auth["M"].add(p.name)
    return auth


def tokens_in_docs():
    """[(file, line_no, raw_token, ns, name)]

    **跳過 ``` 圍欄內的內容**：那些是公式／pseudo-code（例如 `R.prio > T.prio` 的
    requester／target），不是 ID 引用。混在一起檢查只會製造假警報。
    """
    found = []
    pat = re.compile(r"(?<![A-Za-z0-9_])(B\d{2}|[TRLCM]\.([A-Za-z0-9_][A-Za-z0-9_.\-]*))(?![A-Za-z0-9_])")
    for g in DOC_GLOBS:
        for p in ROOT.glob(g):
            fenced = False
            for i, line in enumerate(read(p).splitlines(), 1):
                if line.lstrip().startswith("```"):
                    fenced = not fenced
                    continue
                if fenced:
                    continue
                for m in pat.finditer(line):
                    raw = m.group(1)
                    ns = raw[0]
                    name = m.group(2) if m.group(2) else raw
                    found.append((p.relative_to(ROOT), i, raw, ns, name))
    return found


def main():
    # Windows 主控台預設 cp950，遇到「喺」會拋 UnicodeEncodeError。
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass
    verbose = "--verbose" in sys.argv
    auth = authority()
    missing, unknown_ns = [], []
    seen = set()

    for f, ln, raw, ns, name in tokens_in_docs():
        if (raw, ns, name) in seen:
            continue
        seen.add((raw, ns, name))
        pool = auth.get(ns, set())
        if ns == "C":
            ok = name.upper() in {x.upper() for x in pool}
        elif ns == "M":
            ok = name in pool
        elif ns == "B":
            ok = raw in pool
        else:
            # T/R/L：字串本身要喺 firmware 出現過（前綴前的部分是名字）
            ok = name in pool or name.split(".")[-1] in pool
        if not ok:
            missing.append((f, ln, raw))

    print(f"權威來源：B {len(auth['B'])} 個階段、C {len(auth['C'])} 個指令、"
          f"字串 {len(auth['T'])} 個、模塊 {len(auth['M'])} 個")
    print(f"掃到 {len(seen)} 個不重複 ID token")

    if verbose:
        for f, ln, raw in missing:
            print(f"  ✗ {f}:{ln}  {raw}")

    if not missing:
        print("全部 ID 都指得到 code 裡的東西 ✓")
        return 0

    print(f"\n有 {len(missing)} 個 ID 喺 code 搵唔到（改名／搬走／打錯）：")
    for f, ln, raw in missing[:40]:
        print(f"  {f}:{ln}  {raw}")
    if len(missing) > 40:
        print(f"  …仲有 {len(missing) - 40} 個（用 --verbose 睇全部）")
    return 1


if __name__ == "__main__":
    sys.exit(main())
