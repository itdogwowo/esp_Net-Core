#!/usr/bin/env python3
"""把 WAMR（espressif/wasm-micro-runtime 2.4.0）補到可以喺 ESP-IDF 6 + picolibc 上跑。

為什麼要呢個檔：`firmware/managed_components/` 唔入 git（見 .gitignore），IDF 的
component manager 會喺 configure 階段由 registry 抓一份乾淨的 WAMR 落嚟。上游
2.4.0 對 IDF 6 有幾個位編唔過／跑唔起，所以我哋用 patch 而唔係 vendor 成個
上游（11 MB）：

  ① espidf_file.c   IDF 6 收緊標頭：冇了自己 include <sys/stat.h>；picolibc 冇
                    S_IS* 巨集；renameat 冇 declaration（實作在 espidf_platform.c）。
  ② espidf_platform.c  fstatat() shim 要用 struct stat → 補 <sys/stat.h>。
  ③ espidf_thread.c    os_self_thread() 用 pthread_self()，而 IDF 的 pthread_self()
                    對「唔係用 pthread_create 開的 task」直接 assert。我哋由 FreeRTOS
                    task（nc_mod_wamr_backend 在 ncm 任務裡）呼叫 WAMR，所以要改成
                    用 xTaskGetCurrentTaskHandle()。（handle 只係 identity；THREAD_MGR
                    關閉時 WAMR 只用它做記錄。）
  ④ espidf_memmap.c  冇 PSRAM 的 S3 板（Matrix／SuperMini）會行 MALLOC_CAP_EXEC
                    分支，而 IDF 6 個 macro 要 CONFIG_HEAP_HAS_EXEC_HEAP 才有 →
                    補 include + fallback 去 MALLOC_CAP_INTERNAL（interp 唔需要
                    executable memory）。

呼叫方式：
    tools/patch_wamr.py                       # 自動搵 firmware/managed_components/...
    tools/patch_wamr.py --component <dir>     # 指定 component 目錄
    tools/patch_wamr.py --check               # 只檢查有冇補（0=已補齊、1=未補）

firmware/CMakeLists.txt 在 project() 之後會自動叫呢支（依賴解析完、編譯之前），
所以用 `idf.py build` 定 `make.py build` 都會自動補。腳本 idempotent：已補過的
位會跳過，唔會令 WAMR 每次 configure 都重新編譯。
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_COMPONENT = ROOT / "firmware" / "managed_components" / "espressif__wasm-micro-runtime"

PATCH_MARK = "wamr-idf6-patch"      # 所有補丁段落都帶呢個字（idempotency 靠它）

STAT_INCLUDE_PATCH = """// ── %s：IDF 6 / picolibc ──
// IDF 6 收緊標頭：呢個檔以前靠 platform_api_extension.h 間接拉入 <sys/stat.h>，
// 而 picolibc 冇 S_IS* 巨集。WASI 關閉唔代表可以唔編（platform 係 glob 收檔）。
#include <sys/stat.h>
#ifndef S_ISBLK
#define S_ISBLK(mode) 0
#endif
#ifndef S_ISCHR
#define S_ISCHR(mode) 0
#endif
#ifndef S_ISDIR
#define S_ISDIR(mode) 0
#endif
#ifndef S_ISFIFO
#define S_ISFIFO(mode) 0
#endif
#ifndef S_ISLNK
#define S_ISLNK(mode) 0
#endif
#ifndef S_ISREG
#define S_ISREG(mode) 0
#endif
#ifndef S_ISSOCK
#define S_ISSOCK(mode) 0
#endif
// picolibc 冇 declare renameat（實作在 espidf_platform.c 的 shim）
#ifdef ESP_PLATFORM
int renameat(int oldfd, const char *oldpath, int newfd, const char *newpath);
#endif
""" % PATCH_MARK

SELF_THREAD_OLD = """korp_tid
os_self_thread(void)
{
    /* only allowed if this is a thread, xTaskCreate is not enough look at
     * product_mini for how to use this*/
    return pthread_self();
}
"""

SELF_THREAD_NEW = """korp_tid
os_self_thread(void)
{
    /* %s：IDF 的 pthread_self() 只認 pthread_create() 開的 thread（靠 pthread TLS
     * 找 esp_pthread_t），一般 FreeRTOS task 會直接 assert。WAMR 由 ncm 任務
     * （xTaskCreate）呼叫，所以用 task handle 做 identity。 */
    return (korp_tid)(uintptr_t)xTaskGetCurrentTaskHandle();
}
""" % PATCH_MARK

# 冇 PSRAM 的 S3 板（Matrix／SuperMini）冇 WASM_MEM_DUAL_BUS_MIRROR，os_mmap 會行
# MALLOC_CAP_EXEC 分支；但 IDF 6 的 MALLOC_CAP_EXEC 只喺 CONFIG_HEAP_HAS_EXEC_HEAP
# 之下才定義（S3 預設冇）→ 直接編唔過。WAMR interp 唔需要「可執行」記憶體，
# 退而求其次用內部 RAM 就夠。
MEMMAP_INCLUDE_PATCH = (
    "/* wamr-idf6-patch：冇 PSRAM 的 S3 板行唔到 MALLOC_CAP_EXEC 分支 */\n"
    '#include "esp_heap_caps.h"\n'
    "#ifndef MALLOC_CAP_EXEC\n"
    "#define MALLOC_CAP_EXEC MALLOC_CAP_INTERNAL\n"
    "#endif\n"
)

PATCHES = [
    # (相對路徑, 原文, 補丁後, 已補的偵測字串)
    (
        "core/shared/platform/esp-idf/espidf_file.c",
        '#include "platform_api_extension.h"\n#include "libc_errno.h"\n#include <unistd.h>\n',
        '#include "platform_api_extension.h"\n#include "libc_errno.h"\n#include <unistd.h>\n'
        + STAT_INCLUDE_PATCH,
        PATCH_MARK,
    ),
    (
        "core/shared/platform/esp-idf/espidf_platform.c",
        '#include "sdkconfig.h"\n#include "platform_api_vmcore.h"\n#include "platform_api_extension.h"\n',
        '#include "sdkconfig.h"\n#include "platform_api_vmcore.h"\n#include "platform_api_extension.h"\n'
        "\n// ── %s：fstatat() shim 的參數要用 struct stat ──\n#include <sys/stat.h>\n" % PATCH_MARK,
        PATCH_MARK,
    ),
    (
        "core/shared/platform/esp-idf/espidf_thread.c",
        SELF_THREAD_OLD,
        SELF_THREAD_NEW,
        PATCH_MARK,
    ),
    (
        "core/shared/platform/esp-idf/espidf_memmap.c",
        '#include "platform_api_vmcore.h"\n#include "platform_api_extension.h"\n',
        '#include "platform_api_vmcore.h"\n#include "platform_api_extension.h"\n'
        + MEMMAP_INCLUDE_PATCH,
        PATCH_MARK,
    ),
]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--component", default=str(DEFAULT_COMPONENT),
                    help="WAMR component 目錄")
    ap.add_argument("--check", action="store_true", help="只檢查，不寫入")
    args = ap.parse_args()

    comp = Path(args.component)
    if not comp.is_dir():
        print(f"patch_wamr: {comp} 不存在 → 跳過（冇裝 WAMR）")
        return 0

    pending = 0
    for rel, old, new, marker in PATCHES:
        path = comp / rel
        if not path.is_file():
            print(f"patch_wamr: !! 缺少 {rel}")
            return 1
        text = path.read_text(encoding="utf-8")
        if marker in text:
            print(f"patch_wamr: 已補 {rel}")
            continue
        pending += 1
        if text.count(old) != 1:
            print(f"patch_wamr: !! {rel} 找不到預期原文（上游改過？）")
            return 1
        if not args.check:
            path.write_text(text.replace(old, new), encoding="utf-8")
            print(f"patch_wamr: 補上 {rel}")
        else:
            print(f"patch_wamr: 未補 {rel}")

    if args.check:
        return 0 if pending == 0 else 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
