# 效果載入器：怎麼開發「改檔案就換行為」

> **用途**：回答「我要怎麼**做**這個機制」—— 不是選哪個格式，而是自己動手時要寫哪幾段、檔案長什麼樣、失敗時會怎樣。
> **對象**：要動手寫 `nc_fx_*` 的人。
> **狀態**：設計未實作。格式與載入流程可直接照抄。
> **相關**：[effect-language.md](effect-language.md)（為什麼選 wasm）· [buffer-manager.md](buffer-manager.md)（記憶體從哪來）· [extension-architecture.md](extension-architecture.md)（Component 與帳本）

---

## 0. 一句話

**你要開發的不是「編譯器」，是「載入器」。**

編譯器已經有人寫好了 —— `tinygo build` + `wamrc`，你在 Mac 上 call 它們，一行 shell。真正要自己寫的只有三件事：

1. **一個容器格式**（`.fxe`，48 B 檔頭 + 兩塊 payload）
2. **一個載入器**（驗證 → 要記憶體 → 載入 → init → 交換，約 250 行 C）
3. **一個交換語義**（一個原子指標 + 幀邊界）

**「後期編譯」不是一個新技術，只是把「編譯」從 `idf.py build` 那一刻，搬到「你按下上傳」那一刻。**

---

## 1. 先分清楚「後期」是哪一刻

同一個需求，成本可以差 100 倍。先選時間點：

| 編譯發生在 | 你要寫什麼 | 改完到生效 | 表達力 | 現實評價 |
|---|---|---|---|---|
| **A. 主機端**（上傳前） | **只寫載入器** | 1–3 秒（build + 上傳） | 完整（Go / Rust / C） | ✅ 建議 |
| **B. 裝置端**（上傳時編譯） | 載入器 + **編譯器** | 0.3 秒（只上傳） | 受限（你的語言） | ⚠️ 3–6 週 |
| **C. 載入時**（ELF 重定位） | 載入器 + 連結器 | 1 秒 | 完整（C） | ⚠️ 無沙箱 |
| **D. 執行時**（JIT） | 全部 | 0 | 完整 | ❌ MCU 不現實 |
| **E. 不編譯**（資料驅動） | 只寫求值器 | 0.3 秒 | 受限（節點庫） | ✅ 先做這個 |

**你不能同時要「零編譯」與「完整表達力」。** A 是唯一兩者都接近的：編譯只要 1 秒，而且那 1 秒在 Mac 上，不在裝置上。

> 你現在的痛點其實不是「有編譯」，而是「編譯完要**燒錄**」。把燒錄換成檔案傳輸，痛點就消失了 —— 那件事 A 已經做到。

---

## 2. 四個零件，各自誰負責

| 零件 | 內容 | 誰提供 | 你要寫 |
|---|---|---|---|
| **容器** | `.fxe` = 檔頭 + manifest + payload | 你 | ~100 行 + 格式定義 |
| **載入器** | 驗證 → 配置 → 載入 → init → 交換 | 你 | **~250 行（核心工作）** |
| **執行引擎** | 跑 payload | WAMR（現成 component） | 接 5 個 API |
| **編譯器** | 原始碼 → payload | tinygo / wamrc | ~30 行 shell 包裝 |

**注意「執行引擎」是別人的**。你不會自己寫一個 wasm VM，就像你不會自己寫 TCP/IP。

---

## 3. `.fxe` 格式（48 B 檔頭）

全部 little-endian。`magic` 用四個字元，讓 `file`/hexdump 一眼認得。

| off | size | 欄位 | 說明 |
|---|---|---|---|
| 0 | 4 | `magic` | `'F','X','E','1'` |
| 4 | 2 | `abi` | 目前 0；不符就拒絕 |
| 6 | 2 | `flags` | bit0: 有 AOT；bit1: 有 manifest |
| 8 | 4 | `total_size` | 整個檔案，用來擋截斷 |
| 12 | 4 | `payload_off` | 通常 48 |
| 16 | 4 | `payload_size` | wasm 或 aot 的長度 |
| 20 | 4 | `payload_crc` | crc32；傳輸錯誤在這裡擋 |
| 24 | 4 | `target_id` | 0=wasm（通用）、1=rv32-ilp32f、2=xtensa |
| 28 | 4 | `mem_bytes` | 這個效果要多少線性記憶體 |
| 32 | 4 | `px` | 它假設的像素數（跟 config 不符就拒絕） |
| 36 | 4 | `fps` | 建議幀率（給排程參考） |
| 40 | 4 | `manifest_off` | JSON 的位移（0 = 沒有） |
| 44 | 4 | `manifest_size` | |

```c
_Static_assert(sizeof(nc_fxe_hdr_t) == 48, "fxe header must stay 48 B");
```

**為什麼 AOT 是可選的**：第一版只出 `.wasm`（理由見 [effect-language.md](effect-language.md) §4.4.4）。`flags` bit0 留著，等到某個效果實測 > 8 ms/幀，再為它單獨加 `.aot` —— 格式不用改。

**為什麼要 `target_id`**：同一份 `.wasm` 可以在 P4 與 S3 跑（interp），但 `.aot` 綁架構。檔頭寫清楚，載入器才不會把 riscv32 的機器碼載進 S3。

**為什麼要 `mem_bytes`**：載入前就要知道要跟 arena 要多少，不能「先載入再說」。

---

## 4. 載入器：六步

```c
esp_err_t nc_fx_load(const char *path, nc_fx_t **out)
{
    /* ① 讀檔頭，驗證 */
    nc_fxe_hdr_t h;
    TRY(nc_fs_read(path, 0, &h, sizeof h));
    if (memcmp(&h.magic, "FXE1", 4))        return NC_ERR_BAD_FORMAT;
    if (h.abi != NC_FX_ABI)                 return NC_ERR_ABI;
    if (h.total_size > NC_FX_MAX_FILE)      return NC_ERR_TOO_BIG;
    if (h.px != 0 && h.px != nc_px_count()) return NC_ERR_PX_MISMATCH;
    if (h.target_id != nc_fx_target_id())   return NC_ERR_WRONG_TARGET;

    /* ② 讀 payload 並驗 crc（整塊讀，20 KB 很快） */
    uint8_t *blob = nc_mem_arena_alloc(fx_arena, h.payload_size);
    if (!blob) return NC_ERR_NO_MEM;
    TRY(nc_fs_read(path, h.payload_off, blob, h.payload_size));
    if (crc32(blob, h.payload_size) != h.payload_crc) return NC_ERR_CRC;

    /* ③ 向 arena 要線性記憶體（不是 malloc） */
    void *mem = nc_fx_arena_take(h.mem_bytes);
    if (!mem) return NC_ERR_NO_MEM;

    /* ④ 建 module instance（WAMR） */
    nc_fx_t *fx = calloc(1, sizeof *fx);
    fx->inst = wasm_runtime_instantiate(blob, h.payload_size,
                                        /*stack*/ 8 * 1024, /*heap*/ 0,
                                        mem, h.mem_bytes, &err);
    if (!fx->inst) return NC_ERR_LOAD;

    /* ⑤ 寫 descriptor → fx_init */
    nc_fx_write_desc(fx);
    if (wasm_runtime_call(fx->inst, "fx_init", &rc) != true || rc != 0)
        return NC_ERR_INIT_REJECTED;

    *out = fx;
    return ESP_OK;
}
```

**六步對應六種失敗**，每一種都要有不同的錯誤碼 —— 這樣上傳工具才能告訴你「是格式錯、是 ABI 舊、是記憶體不夠、還是你自己在 `fx_init` 裡回 1」。

### 4.1 為什麼「讀整塊」而不是串流

20 KB 的 payload，從 FAT 讀一次是 5–20 ms。串流載入要有 fallback（失敗了 compat 很難做），而 20 KB 不值得。

### 4.2 記憶體從哪來：`nc_fx_arena_take()`

沿用 `buffer-manager.md` 的規則：

```
開機：nc_plan_declare("fx.arena", group="fx", level=4, bytes=NC_FX_ARENA_MAX) → commit
載入：arena_take(bytes)   ← 只是 bump，不再向系統要
換檔：arena_reset_to(mark) ← 把上一個效果用的切回去
```

**換檔不是 free 再 alloc**，是「回到上一個 mark」—— 不會碎片化，也不會失敗。

---

## 5. 交換：一個原子指標 + 幀邊界

```c
static _Atomic(nc_fx_t *) s_live;      /* compute task 讀，loader 寫 */

/* loader（任何 task） */
nc_fx_t *old = atomic_exchange_explicit(&s_live, next, memory_order_release);
atomic_store(&s_retire_at, frame_id + 2);   /* 兩幀後才真的釋放 */

/* compute task（每幀開頭） */
nc_fx_t *fx = atomic_load_explicit(&s_live, memory_order_acquire);
fx->ops->render(fid, t_us, 0, px);
```

**三個細節**：

1. **`release` / `acquire` 不是可選的**。少了它，編譯器可能把「初始化 module」的寫入排到「發布指標」之後 —— 症狀是「偶爾第一幀是垃圾」。
2. **兩幀後才釋放舊的**：compute task 可能正握著舊指標在跑。用 generation 或簡單的 `frame_id + 2` 都行。
3. **不要在 ISR 裡 swap**。交換發生在 loader task，compute task 只讀。

這跟你 `nc_hub` 的 handoff 是同一個模式：**寫的人發布指標，讀的人拿指標，中間沒有鎖。**

---

## 6. 失敗模式對照表

| 檢查 | 錯誤碼 | 使用者看到什麼 | 系統狀態 |
|---|---|---|---|
| magic 不對 | `BAD_FORMAT` | 「這不是 .fxe」 | 舊效果照跑 |
| abi 不符 | `ABI` | 「效果太新/太舊」 | 舊效果照跑 |
| crc 錯 | `CRC` | 「傳輸壞了，重傳」 | **不留半個檔** |
| px 不符 | `PX_MISMATCH` | 「這個效果是 256 px 的」 | 舊效果照跑 |
| target 不符 | `WRONG_TARGET` | 「AOT 是給另一顆晶片的」 | 可退回 interp |
| 記憶體不夠 | `NO_MEM` | 「arena 滿了，先砍一個」 | 舊效果照跑 |
| init 回非 0 | `INIT_REJECTED` | 「效果自己拒絕（參數錯）」 | 舊效果照跑 |
| render 超時 3 幀 | `TIMEOUT` | 「效果跑不完，已卸載」 | 回上一個 / 黑畫面 |

**這張表就是「不會變磚」的證明。** 八種失敗，沒有一種會讓設備停住。

---

## 7. 如果你真的不想編譯：資料驅動版

一樣的載入器，payload 換成 JSON：

```json
{"nodes": [
  {"op":"noise2d", "scale":3, "speed":1.4, "out":"n"},
  {"op":"palette", "in":"n", "name":"fire", "out":"rgb"},
  {"op":"blur",    "in":"rgb", "radius":1}
]}
```

載入器做的事變成：解析 JSON → 建成節點陣列 → 綁到原生 kernel。

| 項目 | 程式碼 |
|---|---|
| JSON 解析 | 用 cJSON（你已經有） |
| 節點表（12 個 op） | 每個 30–80 行 C |
| 求值器（拓撲排序 + 每像素遞迴） | ~200 行 |
| 合計 | **~500 行，不用編譯器** |

**代價**：只能組合既有的 12 個節點。想要「每幀算一個新奇的東西」就沒門。

**但這 500 行可以吃掉 8 成的需求，而且改完只要傳 2 KB 的 JSON。**

---

## 8. 自己寫語言要多少（誠實估）

如果你堅持要「傳原始碼、裝置自己編」：

| 模組 | 行數 | 難點 |
|---|---|---|
| lexer | 200 | 不難 |
| parser（遞迴下降） | 400 | 不難 |
| 型別檢查 | 200 | 開始難 |
| codegen → bytecode | 400 | 中等 |
| VM（含算術、跳躍、呼叫、陣列） | 600 | 中等，但效能要調 |
| 錯誤訊息 + 行號 + 反編譯 | 300 | 最容易被低估 |
| **合計** | **~2,100 行** | **3–6 週** |

而且這 2,100 行會**一直長**：每個新語法、每個新錯誤訊息、每個除錯需求。

> Pixelblaze 能做這件事，是因為那是他們公司的主力產品，養了好幾年。
> 你現在連 buffer manager 的 P0 都還沒開工 —— 順序不對。

---

## 9. MVP：五個里程碑

每個都能獨立驗收，不用等前一個完美。

| # | 做什麼 | 驗收 | 時間 |
|---|---|---|---|
| **M1** | `.fxe` 格式 + `tools/fx.py pack`（host 端） | `fx.py pack` 產出檔，`fx.py dump` 印得出欄位 | 0.5 天 |
| **M2** | host 端載入器（node，就是 `tools/poc/fx_poc/`） | 換檔、壞檔拒絕，兩個 checksum 不同 | 已做 |
| **M3** | 板上載入器（C + WAMR interp） | 同一個 `.wasm` 上板，checksum 與 host 相同 | 2–3 天 |
| **M4** | 交換語義（原子指標 + 兩幀後釋放） | 邊跑邊換 20 次，不掉幀、不漏記憶體 | 1 天 |
| **M5** | AOT（wamrc + target 檢查） | 同畫面、≤ 2 ms/幀 | 1 天 |

M3 是唯一有外部風險的（WAMR component 對 IDF v6.0.3）。其餘都是純工程。

---

## 10. 服務化：該註冊的是三個東西，不是一個

> 這個問題踩在一個很自然的直覺上：「把它註冊成一個服務，就能像 TFT / LVGL 一樣插拔」。方向對，但 **一層不夠、一層又太多**。

### 10.1 三層，各自的生命週期不同

| 層 | 是什麼 | 生命週期 | 數量 | 註冊去哪 |
|---|---|---|---|---|
| **wasm** | runtime 能力（init / instantiate / 查錯誤） | 整個 boot，**單例** | 1 | 服務（或全域 `nc_wasm_*`） |
| **fx** | 政策：arena + live 指標 + 看門狗 + render task | boot → 可停 | 1 | 服務 |
| **module** | 一個效果實例 | 載入 → 卸載 | N | `fx` 底下的實例表，**不是服務** |

**為什麼效果不該註冊成服務**：

1. 服務註冊表是 boot-time 的扁平清單；效果是 runtime 產生的。混在一起，`status` 會變成一坨噪音。
2. 50 個效果 = 50 個服務項，查找變成線性掃描。
3. 服務沒有「卸載」語義（你的 `register_service` 只有 register / get）。

這跟 `rx-architecture.md` 的結論一模一樣：**註冊表存 ID，引擎/實例可以隨時增減。**

### 10.2 介面（一個 ops struct 就夠）

```c
typedef struct {
    esp_err_t (*init)(void *arena, size_t bytes);   // WAMR init，一次
    esp_err_t (*load)(const char *path, nc_fx_t **out);
    esp_err_t (*unload)(nc_fx_t *fx);
    esp_err_t (*select)(const char *name);          // 切 live，下一幀生效
    nc_fx_t  *(*live)(void);                        // render task 每幀呼叫
    size_t    (*status_json)(char *buf, size_t cap);
} nc_fx_ops_t;
```

**不需要先建一個通用 service registry。** 你只有一個 `fx`，全域 accessor（`nc_fx()`）就夠；等出現第三個可插拔子系統再抽。註冊表的價值在「多個同類」，你現在是「一個」。

### 10.3 開機接線（全部用你現有的東西）

```c
storage_init();                     // 隨身碟（/data 掛載）
nc_plan_declare(&(nc_plan_decl_t){
    .service = "fx", .name = "fx.arena", .level = 4,
    .cls = NC_PLAN_PSRAM, .bytes = NC_FX_ARENA_MAX });
nc_plan_commit(avail, /*allow_shed*/ true);
nc_fx_init(nc_plan_get("fx.arena"));          // WAMR init，只有一次
nc_task_register(&(nc_task_def_t){
    .name = "fx", .fn = nc_fx_task, .core = 1, .layer = 3, ... });
```

`nc_plan_decl_t` 本來就有 `service` 欄位（「誰要的」）—— **服務這個概念在你的 C code 裡已經存在了**，只是還沒有對應的 API 介面。這一步不是發明，是把欄位補上行為。

### 10.4 檔案系統就是效果的註冊表

```
/data/fx/fire26.fxe
/data/fx/plasma.fxe
/data/fx/rainbow.fxe
```

- 「有哪些效果」= `ls /data/fx/`，不需要另一張表
- 「上傳一個新效果」= NC4 FILE_* 寫進去（staging → validate → rename）
- 「刪掉」= unlink
- 「壞掉的」= 開機掃一遍，載入失敗的標記、不啟用（不刪，留給你看）

**這是「分離式固件」最省事的形態**：檔案系統已經有交易語義（staging + atomic rename），你不需要再建一套。

### 10.5 「動態彈性」的邊界

服務化會讓下面這些變動態 —— 但不會更多：

| 可動態 | 怎麼做 |
|---|---|
| 有哪些效果 | 檔案放進 `/data/fx/` |
| 哪個在跑 | `nc_fx_select("fire26")` |
| 參數 | descriptor 後的參數區 |
| 開 / 關 | `enable/disable`，task 閒置不耗 CPU |
| 卸載 | `unload`，兩幀後釋放 |
| 記憶體回收 | arena mark / reset |

| 仍然靜態 | 為什麼 |
|---|---|
| arena 總量 | 開機 commit 就定死（`nc_plan` 的不變量） |
| 每幀預算 | 排程參數，改了就重啟 |
| 線性記憶體大小 | wasm 的 page 粒度，載入時定 |
| ABI | 編譯期 |

**最後那四行就是「不會 GC 停頓、不會變磚」的來源。** 想把它們也做成動態，就等於回到 MicroPython —— 那正是你離開它的理由。

### 10.6 五個坑

| # | 坑 | 症狀 | 對策 |
|---|---|---|---|
| 1 | init 順序錯（WAMR 在 plan commit 之前） | 拿不到記憶體，或 heap 被切碎 | 照 §10.3 的順序，寫進 `nc_tasks_boot` 的註解 |
| 2 | `wasm_runtime_init` 被呼叫兩次（soft-reboot） | 第二次直接崩 | 用 flag 守；跟 mp 版「C 層沒清」同一個教訓 |
| 3 | 兩個 task 同時呼叫同一個 instance | 隨機資料損毀 | 一個 `fx` task 跑全部效果；render 只有一個進入點 |
| 4 | 卸載時 render 還在跑 | use-after-free | `frame_id + 2` 之後才 `deinstantiate` |
| 5 | WAMR 自己也要 heap（module heap / stack） | 跑到一半 OOM | 從 plan 要一塊 `fx.runtime`（DMA 不要，INTERNAL 即可） |

---

## 11. 決策記錄

| 方案 | 為什麼不用 |
|---|---|
| 自己寫 wasm VM | 已經有人寫好，而且 AOT 需要 LLVM backend |
| 自己寫語言（B） | 2,100 行 + 無限期維護；除非 P0 全失敗，否則不值得 |
| 用 tar/zip 當容器 | 要解壓到 RAM 才能驗 crc；`.fxe` 可以邊驗邊載 |
| 每次換檔都 reset 整個 arena | 會把常駐的其他效果一起清掉；用 mark/reset 只退自己那塊 |
| 在 ISR 裡 swap | 會跟 compute 打架；swap 只在 task 裡做 |
| 把 payload 放 flash 直接 XIP | 可行，但要 WAMR 的 XIP 支援（見 effect-language §7），列 P2 |
| **每個效果註冊成一個服務** | 服務是 boot-time 扁平清單、沒有卸載語義；效果是 runtime 實例（§10.1） |
| **先建一個通用 service registry** | 目前只有 `fx` 一個，全域 accessor 就夠；等第三個可插拔子系統出現再抽 |

---

## 12. 參考

- 檔頭設計參考：ESP-IDF `esp_app_format`（`esp_app_desc_t`）
- 交換模式：本專案 `nc_hub` 的 handoff（`buffer-manager.md` §6）
- 記憶體：`buffer-manager.md` §4–§5（declare → commit → take）
- 執行引擎 API：WAMR `wasm_runtime_instantiate` / `wasm_runtime_call_wasm` / `wasm_runtime_module_malloc`
