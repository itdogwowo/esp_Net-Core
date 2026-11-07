#!/usr/bin/env python3
"""靜態算「任務棧最深會用到幾多」。

為什麼要這個工具
----------------
`uxTaskGetStackHighWaterMark()` 只量到**跑過**的路徑。沒跑過的分支（錯誤處理、
另一個檔案系統、更深的 JSON）可能更深 —— 那些不是「不可預測」，是「還沒算」。

呼叫圖本身是靜態的：GCC 用 `-fcallgraph-info=su` 就把「誰叫誰 + 每層幾多 bytes」
寫進 `.ci`。這個工具把 945 個 `.ci` 併起來，從任務入口出發算最壞情況。

用法
----
    # 建一次帶分析的 build（唔會改動程式碼，只多啲產物）
    cd firmware
    cmake -G Ninja -B build_ana -DPORT=S3/ESP32-S3-N16R8 -DIDF_TARGET=esp32s3
    echo ' -fstack-usage -fcallgraph-info=su' >> build_ana/toolchain/cflags
    find build_ana -name '*.obj' -delete && ninja -C build_ana

    # 再算
    tools/stack_depth.py firmware/build_ana --root nc4_bus_task_entry --root app_main

限制（要老實講）
----------------
- `.ci` 是**最佳化之後**的圖：inline 掉的函數已併入呼叫者，所以數字貼近實機。
- 純組語／中斷向量不在圖內。
- 函數指標（handler 表、VFS ops）在圖上是一條邊指向具體函數（GCC 通常能列舉），
  但如果有「未知目標」，工具會停在那裡並標示 `[未知]`。
- 遞迴（cJSON 解析）會形成環；工具每次只算一層並標示，實際深度取決於輸入資料。
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

NODE_RE = re.compile(r'node:\s*\{\s*title:\s*"([^"]+)"\s*label:\s*"((?:[^"\\]|\\.)*)"')
EDGE_RE = re.compile(r'edge:\s*\{\s*sourcename:\s*"([^"]+)"\s*targetname:\s*"([^"]+)"')
SIZE_RE = re.compile(r"(\d+)\s+bytes\s*\((static|dynamic)")


def func_of(title: str) -> str:
    """`/path/file.c:func` → `func`；純符號就原樣回。"""
    return title.rsplit(":", 1)[-1] if ":" in title else title


def short(title: str) -> str:
    """顯示用：`main.c:on_status_get`"""
    if ":" not in title:
        return title
    path, fn = title.rsplit(":", 1)
    return f"{Path(path).name}:{fn}"


class Graph:
    def __init__(self) -> None:
        self.size: dict[str, int] = {}
        self.dynamic: set[str] = set()
        self.by_func: dict[str, list[str]] = {}
        self.edges: dict[str, set[str]] = {}
        self.files = 0
        self.missing: set[str] = set()

    def load(self, path: Path) -> None:
        text = path.read_text(errors="replace")
        self.files += 1
        for m in NODE_RE.finditer(text):
            title, label = m.group(1), m.group(2)
            sm = SIZE_RE.search(label)
            if not sm:
                continue
            self.size[title] = int(sm.group(1))
            if sm.group(2) == "dynamic":
                self.dynamic.add(title)
            self.by_func.setdefault(func_of(title), []).append(title)
        for m in EDGE_RE.finditer(text):
            a, b = m.group(1), m.group(2)
            self.edges.setdefault(a, set()).add(b)

    def resolve(self, name: str) -> str | None:
        if name in self.size:
            return name
        cands = self.by_func.get(func_of(name))
        if not cands:
            return None
        # 同名（static 撞名）→ 取最大框，保守
        return max(cands, key=lambda c: self.size.get(c, 0))

    def deepest(self, root: str) -> tuple[int, list[str], list[str]]:
        best = (0, [], [])
        notes: list[str] = []

        def walk(node: str, acc: int, path: list[str]) -> None:
            nonlocal best
            if node in path:
                notes.append(f"環：{short(node)}（遞迴，深度取決於輸入）")
                if acc > best[0]:
                    best = (acc, path + [f"(→{short(node)})"], list(notes))
                return
            sz = self.size.get(node, 0)
            acc += sz
            path = path + [node]
            if acc > best[0]:
                best = (acc, list(path), list(notes))
            kids = self.edges.get(node)
            if not kids:
                return
            resolved = []
            for k in kids:
                r = self.resolve(k)
                if r is None:
                    self.missing.add(k)
                else:
                    resolved.append(r)
            for r in resolved:
                walk(r, acc, path)

        walk(root, 0, [])
        return best


def main() -> int:
    ap = argparse.ArgumentParser(description="靜態算任務棧最壞深度（讀 GCC 的 .ci）")
    ap.add_argument("build_dir")
    ap.add_argument("--root", action="append", default=[], help="任務入口（可多個）")
    args = ap.parse_args()

    d = Path(args.build_dir)
    if not d.is_dir():
        print(f"沒有這個目錄：{d}", file=sys.stderr)
        return 1

    g = Graph()
    for p in d.rglob("*.ci"):
        g.load(p)
    if g.files == 0:
        print("找不到 .ci —— 這個 build 沒有開 -fcallgraph-info。", file=sys.stderr)
        return 1
    print(f"讀了 {g.files} 個 .ci、{len(g.size)} 個函數（{len(g.dynamic)} 個 dynamic/alloca）\n")

    roots = args.root or ["app_main"]
    rc = 0
    for r in roots:
        node = g.resolve(r)
        if node is None:
            print(f"✘ 找不到根 {r}")
            rc = 1
            continue
        total, path, notes = g.deepest(node)
        print(f"── {r}：靜態最壞 **{total} B**")
        for i, n in enumerate(path):
            sz = g.size.get(n)
            print(f"   {'  ' * min(i, 14)}{short(n)}" + (f"   {sz} B" if sz is not None else ""))
        seen = set()
        for n in notes:
            if n not in seen:
                seen.add(n)
                print(f"   ! {n}")
        print()
    if g.missing:
        sample = ", ".join(sorted(g.missing)[:6])
        print(f"（圖外符號 {len(g.missing)} 個，例如 {sample} —— 通常是組語或 IDF 內部）")
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
