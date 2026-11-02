# 緩衝管理器（Buffer Manager）設計

> **用途**：esp_Net-Core 的緩衝區管理完整設計 —— 目標、現況、架構、四種機制、三層 tier、撇除規則、S3 記憶體實測、分期計畫、待決事項。
> **對象**：要接手實作的人（含未來的自己）；要 review「為什麼不直接用 malloc」的人。
> **狀態**：**設計定案、尚未實作**。現有程式碼只有 pool / arena 兩個原語（見 §2）。
> **相關**：[memory.md](memory.md)（原語與壽命）、[architecture-flow.md](architecture-flow.md)（執行期流程）、[component-layout.md](component-layout.md)（分區）、[roadmap.md](../00_pm/roadmap.md)（分期）

---

## 0. 從哪裡接手

要動工就從 **P0** 開始（見 §12），它不需要板子：

1. 定 `nc_mem_decl_t` / `nc_mem_token_t` / `nc_tier_t`
2. 寫 `nc_mem_declare()` / `nc_mem_commit()`（含排序、加總、撇除、印表）
3. 寫 `tools/hosttest/plan_test`（host 編譯，餵 N 組 config，印總量）

板上只要做一件事：`heap_caps_print_heap_info(MALLOC_CAP_INTERNAL)`，把 `free` / `largest` 貼回 §10，把 cap 填成實測值。

---

## 1. 目標與定位

> **一個 image 裝全部功能，開機讀 config 才決定「這次有哪些任務、各要多少記憶體」；但熱路徑的行為與成本必須在編譯期可證明、在執行期完全確定。**

這是把 linker script 從 build-time 搬到 boot-time 的做法（同 Linux device tree、Android GKI、Zephyr board variants 的思路）。

與 MicroPython 的對照 —— 兩條軸要分開看：

| 軸 | MicroPython | 本專案 |
|---|---|---|
| **行為（程式碼）的動態性** | 高：.py 拖上去就生效 | 低：編譯進 firmware，靠 config 選 |
| **佈局（緩衝）的動態性** | 低：開機定死、硬編碼、`num_buffers=10` | **高：由 plan 推導、可撇除、可回收** |

MicroPython 的「動態記憶體」是 GC heap，不是 buffer manager：大小任意、時間不可預測、沒有 owner、沒有 DMA class。本專案要的是後者。

**三個好處（closed-world 才有）**：

1. 可證的最壞情況（`_Static_assert` 擋 cap）
2. 沒有 GC 停頓
3. plan 的計算可以寫成純函式 → 在 Mac 上窮舉測完所有 config 組合

---

## 2. 現況（程式碼）

| 檔案 | 內容 | 行數 |
|---|---|---|
| `firmware/components/sys/mem/nc_mem.c/h` | **POOL**：`create / get / put / stats`，free list + 臨界區，`heap_caps_aligned_alloc` 一次要 `block × count`<br>**ARENA**：`create / alloc / reset / used / size`，bump | ~213 |
| `firmware/components/sys/mem/nc_mem_cfg.c/h` | 讀 `config.json` 的 `"Buffer"`：`frame.block` / `frame.count` / `assets.size`；clamp 32 KB / 8 / 512 KB | ~90 |
| `firmware/main/main.c` | 建 frame pool（512 B × 4）＋ asset arena（64 KB PSRAM）＋ 開機煙霧測試 | — |

三種 heap class 已分好：`NC_MEM_DMA` / `NC_MEM_INTERNAL` / `NC_MEM_PSRAM`。

**這是原語，不是管理器。** 沒有 plan、沒有 declare/commit、沒有 handoff、沒有 lease、沒有撇除、沒有狀態回報。

### 六個已知問題

| # | 問題 | 後果 |
|---|---|---|
| 1 | `frame.block` 預設 **512 B** | 一幀（676 px × 4）= 2704 B，**這個 pool 連一幀都放不下** |
| 2 | `pool_get(pool, ms)` 內部 `vTaskDelay(1ms)` | 熱路徑用非 0 timeout → 掉幀變成多毫秒卡頓 |
| 3 | 沒有 owner / generation | 忘 `put` → 「跑幾分鐘 pool 空了」，查不出是誰 |
| 4 | `Buffer.frame.block` 與 `System.num_pixels` 兩套來源 | 改 px 不會改 block → 靜默壞掉 |
| 5 | `block_size` 只 4-byte 對齊 | DMA / cache line 建議 32 |
| 6 | cJSON 解析在 carve 之前 | 吃掉一點 `largest_free_block`（小） |

---

## 3. 架構：四個動詞

```
┌─ L4 reclaim  生命週期：lease / baseline verify / reaper ── 只給 Tier 2
├─ L3 move     傳遞：handoff（4 角色）/ ring / queue / snapshot
├─ L2 decide   決策：declare → commit（含撇除）→ token
└─ L1 own      持有：carve / pool / arena / static
```

**核心規則：配置只發生在 L2（開機一次）。L3 之後只有「指標轉手」。**

---

## 4. 三階段生命週期

```
① 讀 config → 驗證 → 唯讀快照（modules 只看這份，不准自己讀檔）
② 每個線上模組 init → nc_mem_declare()      ← 只登記，不配置
③ nc_mem_commit()
     排序 (level, name) → 加總 → 比 largest_free_block
     → 超過就按 Level 撇除（以 feature 為單位，含依賴）
     → carve → 發 token → 印表
④ 任務 start → nc_mem_take(token)           ← 查表；Tier 2 則向 pool 租
⑤ USB / WiFi → 驗 floor
```

| 階段 | 誰做 | 有沒有配置 |
|---|---|---|
| ② declare | 各模組自己 | ❌ |
| ③ commit | 管理器 | ✅ **唯一一次** |
| ④ take | 任務啟動 | Tier 0/1 查表；Tier 2 `try_get` |

**宣告窗口在 commit 時關閉**；commit 之後再 declare → 明確拒絕（不是 assert）。

### 宣告的形狀

```c
typedef struct {
    const char    *name;      // "pixel.frame"（邏輯名）
    const char    *group;     // feature 名，撇除以 group 為單位
    int            level;     // 撇除優先序（0 = 不可撇）
    nc_mech_t      mech;      // OWNED / HANDOFF / QUEUE / SNAPSHOT
    nc_mem_class_t cls;       // DMA / INTERNAL / PSRAM
    size_t         bytes;     // 模組自己算（輸入只能來自 config 快照）
    nc_life_t      life;      // BOOT / RELOAD / TASK
} nc_mem_decl_t;

nc_mem_token_t nc_mem_declare(const nc_mem_decl_t *decl);   // 回 token（不透明）
void          *nc_mem_take(nc_mem_token_t tok, nc_mech_t expect); // 被撇除 → NULL
```

**沒有通用的 `nc_task_bufs_t`**：模組用自己的型別包 handle，只從管理器拿記憶體。

---

## 5. 編譯器的角色：只有一個上限

```c
#define NC_MEM_CAP_DMA    (160 * 1024)   // 待實測替換（見 §10）
#define NC_MEM_CAP_PSRAM  (2 * 1024 * 1024)

_Static_assert(NC_MEM_CAP_DMA <= 160 * 1024,
               "cap 超過內部 RAM 安全值（S3 512 KB，要留給 WiFi/USB/task stack）");
```

**總量不在編譯期算。** 之前考慮過用 X-macro 中央表在編譯期加總，被否決：它加總的是 `NC_MAX_PX` 這種**猜測的上限常數**，證明的是假設不是事實。改成：

- 編譯期：只擋「cap 本身設得太離譜」
- 執行期：用**真實宣告**的總和 vs **實測**的 `largest_free_block`

---

## 6. 四種機制（子系統可以很多，機制只有四種）

| 機制 | 語義 | 判斷句 | 用途 | 失敗模式 |
|---|---|---|---|---|
| **OWNED** | 一塊自己的區域，生命週期＝持有者 | 只有我在用，而且一直都在 | framebuffer、parser、DMA staging | 沒有（開機配好） |
| **HANDOFF** | N 槽 + seq，最新優先 | 一直有新資料覆蓋舊的 | 幀、display flush、stream | 丟幀（可接受） |
| **QUEUE** | 小訊息 + 關聯，不可遺失 | 每一則都要處理 | 跨核控制 | 滿了要背壓 |
| **SNAPSHOT** | 唯讀不可變，換指標 | 讀多寫少，不能看到半套 | config / plan / status | 沒有 |
| （ARENA） | reload 時整塊 reset | reload 才換的東西 | assets、params、波形 | 放不下 → 保留舊版 |

### HANDOFF：4 角色不是環形

```
        writer (core 1)                      reader (core 0)
   BACK  ┌ 正在算 N+1 ┐
 READY  ┌ 算完待送   ┐ ──(seq<<2)|idx──►  取走
 FRONT  ┌           ┐                     DMA 傳輸中
  LAST  ┌ 上一幀     ┐                     逾時重播用
```

| 事件 | 轉移 |
|---|---|
| `write_commit()` | BACK → READY；舊 READY 若沒人取 → 回 BACK（`dropped++`） |
| `read_take()` | READY → FRONT；舊 FRONT → LAST；舊 LAST → BACK |

同步只有一個 32-bit 原子字 `(seq << 2) | slot_index` 加一個 `in_use` bitmask：

```c
_Atomic uint32_t pub;      // writer 只寫，reader 只讀
_Atomic uint32_t in_use;   // reader 標記持有的 slot
```

- writer：挑 `in_use` 未標的 slot → 寫 → `__atomic_nc_store_n(&pub, …, RELEASE)`
- reader：`__atomic_load_n(&pub, ACQUIRE)` → seq 變了才取 → DMA 完成後清 bit

**沒有鎖、沒有環繞運算、沒有 `%`、沒有狀態陣列。** 一次交易約 20–30 cycles（< 0.15 µs）。

> ⚠️ `volatile` **不是** barrier。XTensa 會重排 store；「先寫資料再推 head」必須靠 release/acquire。

---

## 7. 三層 tier 與「關閉」的語義

| Tier | 例子 | 記憶體 | 可停？ | 可回收？ |
|---|---|---|---|---|
| **CORE** | nc4 decode、log、storage、task 監督 | static + 開機 | ❌ | — |
| **CONDITIONAL** | output（RMT/SPI）、USB、WiFi、I2S | 開機 carve | 只能**閒置** | ❌（硬體不可安全重入） |
| **OPTIONAL** | pixel compute、stream、pixel_lab、UI、web | pool 租賃 | ✅ | ✅ |

**判準（三條機械規則）**

1. 被任何 CORE 同步呼叫 → 必須是 CORE（例：`log`）
2. 持有不可安全重入的硬體（USB PHY、LCD/SPI DMA queue、I2S、RMT）→ 至少 CONDITIONAL
3. 只有「記憶體能 100% 歸還」的才算 OPTIONAL —— 驗證方式：`stop()` 後 pool 的 `used` 必須回到 `start()` 前的 baseline

### 「關閉」其實混了兩件事：省 CPU / 省記憶體

| 級 | 做什麼 | 記憶體 | 重啟成本 | 適用 |
|---|---|---|---|---|
| **L1 閒置** | task 存在，block 等訊號，0% CPU | 全留 | < 1 ms | pixel compute、stream、schedule |
| **L2 卸載** | `vTaskDelete`、stack 還 slab、ring 排空還 slot | 回收 slot + stack | 數 ms | pixel_lab、臨時 producer |
| **L3 重載** | 連 arena 都 reset，參數下次從 FAT 讀 | 全回收 | 10–100 ms | 換 asset、換機型、recovery |

**建議第一版只做 L1**，`verify` 照做（在停機時驗）。多數「關閉」要省的是 CPU，不是那 11 KB。

### 關閉的完整動作（四步，順序不可換）

```c
esp_err_t nc_task_stop(const char *name)
{
    atomic_store(&t->stop_req, true);      // ① 請 producer 自己停（不能從外面砍）
    wait_exit(t, 1000);
    nc_handoff_drain(&t->hub);             // ② 排空：還掛著的 slot 全還 pool
    vTaskDelete(t->handle);                // ③ 刪 task（stack 還 slab）
    return nc_lease_verify(name, t->baseline);  // ④ 不符就報錯（debug: abort）
}
```

---

## 8. 撇除（shedding）

當 `total > 可用` 時，按 Level 從最不重要開始撇除**整個 feature**：

```
[plan] 需求 178 KB > 可用 120 KB
[plan] shed: pixel_lab  (-8.1 KB)   L4
[plan] shed: stream     (-10.8 KB)  L4
[plan] shed: web        (-24.0 KB)  L3
[plan] 需求 105 KB ≤ 120 KB  ✓
[plan] WARN: 已撇除 3 個功能，請檢查 config 或降低 px/解析度
```

**五條規則**

1. 以 **feature（group）** 為單位撇，不是單一宣告
2. 撇除要處理**依賴**（撇 pixel 不能留下 render）
3. **確定性**：同 Level 按 group 名排序，宣告順序不影響結果
4. 撇完**重新計算**，直到塞得下或只剩 L0（L0 由 `_Static_assert` 保證一定塞得下 → 迴圈必終止）
5. 撇除結果**必須回報**：進 `0x11xx STATUS` 的 `shed[]`；「config 關掉」與「塞不下被撇」要分得出來

**Level 建議**

| Level | 內容 | 撇除順序 |
|---|---|---|
| 0 | storage、nc4 decode、log、task 監督 | **永不撇除** |
| 1 | output、USB | 最後 |
| 2 | pixel compute、audio | 先 |
| 3 | UI、web | 更先 |
| 4 | pixel_lab、bench、stream | **最先** |

---

## 9. 記憶體規則

| 規則 | 理由 |
|---|---|
| **寫獨佔**（每行拿到不重疊 offset） | 由構造保證獨立，不靠約定 |
| **讀走 peek**（`nc_handoff_peek` / snapshot / seqlock） | 不給裸指標 → 不會撕裂、不會讀到半套 |
| **pool 內先到先得，pool 間預先切分** | 前者是日常作業，後者是決策（要能解釋、能 review） |
| **熱路徑永不等待**（timeout 一律 0） | `vTaskDelay(1ms)` 會把掉一幀放大成掉好幾幀 |
| **一個 pool 一個用途**，不要讓兩個子系統搶 | 否則 `fails` 查不出來源 |
| `block_size` 對齊 **32** | cache line / DMA |
| **共享索引絕不放 PSRAM**；RMT DMA 不可指 PSRAM | 幀緩衝必須 `NC_MEM_DMA` |
| 跨核訊息不准帶「指向租賃記憶體的裸指標」 | 只有兩種合法形式：內嵌 ≤64 B，或連所有權一起移交 handle |
| 一個 block 一個 owner（不共享所有權） | 避免跨核 refcount；兩人都要就給兩塊 |

**禁止清單**

- ❌ 跨核 mutex 保護熱路徑
- ❌ 跨核 `shared_counter++`
- ❌ 熱路徑 `malloc/free`
- ❌ 把 `volatile` 當同步原語
- ❌ 每個子系統自己寫一套 hub（C 版只准一份 `nc_handoff`）

---

## 10. S3 的實際記憶體（實測後回填）

**來源**：ESP-IDF v6.0.3 `components/heap/port/esp32s3/memory_layout.c` 與 `esp_system/ld/esp32s3/memory.ld.in`。

| 起始 | 大小 | 類型 |
|---|---|---|
| `0x3FC88000` | 32 KB | DIRAM（可作 trace mem） |
| `0x3FC90000` | 64 KB | DIRAM |
| `0x3FCA0000` | 64 KB | DIRAM |
| `0x3FCB0000` | 64 KB | DIRAM |
| `0x3FCC0000` | 64 KB | DIRAM |
| `0x3FCD0000` | 64 KB | DIRAM |
| `0x3FCE0000` | ~37.8 KB | DIRAM（尾端被 ROM stack 截斷，到 `0x3FCE9710`） |
| `0x3FCE9710` | ~26.2 KB | DIRAM（ROM 保留，`app_main` 後回收） |
| `0x3FCF0000` | 32 KB | DRAM（DCache 32 KB 時才有） |

前 8 條相鄰同型 → **heap 初始化時合併**：

```
0x3FC88000 → 0x3FCE9710 = 0x61710 ≈ 390 KB
```

扣掉 `.data / .bss / .noinit` 後，**開機 `largest_free_block` 約 300–350 KB**。

### 會把連續塊切碎的四件事

1. 開 trace memory（從 32 KB 邊界切走）
2. DCache 設 64 KB（`0x3FCF0000` 那 32 KB 消失）
3. **太晚要**（WiFi/USB/task 起來之後，largest 掉到幾十 KB）
4. IRAM 用量（DIRAM 與 IRAM 是同一塊實體）

### 所以 carve 的規則

```
① FAT mount 用 static buffer（幾 KB）→ 讀 config
② 立刻 carve / 建 pool（此時 largest 還有 ~300 KB）
③ 之後才起 WiFi / USB / tasks
```

**先檢查再要**：

```c
size_t want = plan_total_bytes();
size_t have = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
if (want > have) { shed_until(want <= have); }
```

> `NC_MEM_CAP_DMA = 160 KB` 是**暫定值**，要換成實測。量測：
>
> ```c
> heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
> ESP_LOGI(TAG, "free=%u largest=%u min=%u", …);
> ```

---

## 11. 決策記錄（討論中被推翻的想法）

| 原本 | 改成 | 為什麼 |
|---|---|---|
| X-macro 中央表 + 編譯期加總 | 編譯器只管 cap，總量開機算 | 加總的是猜測常數，證明的是假設不是事實 |
| 一次 carve 一大塊 | 開機早期連續建多個小 pool（或大 carve 但必須早） | 最大的連續塊比總量小且越晚越小 |
| 通用 `nc_task_bufs_t` + `bind()` | 模組自己的型別 + token 查表 | 那是 MicroPython `ctx` dict 的翻版，C 版不需要 |
| 幀用環形緩衝（深度 10，照抄 mp） | 4 槽 4 角色 handoff | 舊幀沒有價值；環繞運算在多數情境用不到 |
| 「每個服務一個區域」 | 每個記憶體 class 一塊 carve，服務拿 offset | 避免對齊浪費與碎片 |
| 讓服務自己計算並 `get` | 宣告式描述子 + 管理器彙總 | 命令式配置會讓最壞情況算不出來 |

---

## 12. 實作分期

| 階段 | 內容 | 估算 | 驗收 |
|---|---|---|---|
| **P0** | `nc_mem_decl_t` / `token` / `tier`；`declare / commit / shed / dump`；`tools/hosttest/plan_test`（host） | ~330 行 | Mac 上跑 500 組 config（含邊界）全過；印出每組總量與撇除結果 |
| **P1** | `try_get`、`used()`、block header（magic/owner/seq）、32 B 對齊、carve | ~140 行 | 開機印 plan 表；1 小時 `fails=0` |
| **P2** | `handoff`（4 角色）、`ring`、`lease` / `baseline` / `reaper` | ~370 行 | consumer 故意慢 10×：producer 不掉速、`dropped` 增加、canary 無撕裂；互換 core 行為不變 |
| **P3** | `queue`（含關聯 ID）、`snapshot` | ~230 行 | 跨核請求/回覆不遺失；讀 config 不會讀到半套 |
| **P4** | `0x11xx` 狀態：plan hash、shed[]、各 pool/channel 統計 | ~80 行 | 主機看得到「這台是什麼佈局、還健康嗎」 |

**合計約 1,150 行。P0 完全不需要板子。**

---

## 13. 待決事項

| # | 問題 | 建議 |
|---|---|---|
| 1 | Tier 2 支援到哪一級（L1 閒置 / L2 卸載 / L3 重載） | 第一版只做 L1，但 `verify` 照做 |
| 2 | 音訊 ring 是 eager 還是 leased | 若音訊模組可能整塊拔掉 → leased |
| 3 | frame 槽數 3 還是 4 | **4**（含 LAST 重播），對上 `memory.md` 的「compute / output / 上一幀 / 餘裕」 |
| 4 | 撇除之外要不要「縮小」（UI 帶狀 24→12 行） | 先只做撇除，縮小等真的需要 |

---

## 14. 名詞對照（mp_Net-Core → 本專案）

| mp | C | 備註 |
|---|---|---|
| `AtomicStreamHub` | `nc_handoff` / `nc_ring` | C 版沒有 copy/view 雙模式，只有指標 |
| `buffer_hub.alloc_dma` | `NC_MEM_DMA` class | C 版沒有 heap_caps fallback |
| `bus.get_service()` / `ctx` dict | `nc_mem_take(token)` + 模組自己的型別 | C 版可以 early binding |
| `u8_rx_slots`（config） | `nc_ring_depth`（config） | 概念相同 |
| `num_buffers=10`（硬編碼） | 4（handoff）/ 2–4（stream） | 舊幀沒有保留價值 |
| GC | 不存在 | 全部 pool / arena |
| `TaskManager` 的 layer | `nc_task_loop_t.layer` | 已實作 |
