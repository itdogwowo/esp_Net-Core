# 記憶體管理（原語層）

> 原則：**熱路徑不 malloc。** 會隨時間反覆發生的東西（幀、封包、DMA 緩衝）用 pool；
> 一次配、一次放的東西（assets、config、解析結果）用 arena。

> 📌 **本文件只講兩個原語（pool / arena）與它們的壽命。**
> 完整的「緩衝管理器」設計（宣告 → 提交 → 撇除 → token、四種機制、三層 tier、S3 記憶體圖、分期計畫）
> 在 **[buffer-manager.md](buffer-manager.md)**。兩者的關係：
> `memory.md` 是「記憶體怎麼持有」，`buffer-manager.md` 是「誰在什麼時候拿到哪一塊」。

---

## 1. 兩個原語，對應兩種壽命

| | 配置 | 釋放 | 用在 |
|---|---|---|---|
| **POOL** | 固定大小區塊，一次向系統要 | 單塊、成對 | 幀緩衝、DMA 緩衝、封包暫存 |
| **ARENA** | bump 前進 | **一次全部** | assets、config、JSON 解析結果 |

為什麼不直接用 `malloc`：

1. 50 FPS 跑幾小時，`malloc/free` 會把 heap 切碎，某天就配不出連續塊 —— 而且症狀是「跑了三小時才掉幀」，最難查的那種。
2. PSRAM 的配置時間不確定，會直接變成掉幀。
3. 出事時看不出「誰吃掉了記憶體」；pool 有名稱、有 peak、有 fails。

---

## 2. 三種記憶體 class（由硬體限制決定，不是偏好）

| class | 來源 | 特性 | 誰必須用它 |
|---|---|---|---|
| `NC_MEM_DMA` | 內部 RAM | DMA 可用、最快 | RMT / SPI / I2S / LCD 的緩衝 |
| `NC_MEM_INTERNAL` | 內部 RAM | 快，只有幾百 KB | 佇列、協定緩衝、小資料 |
| `NC_MEM_PSRAM` | 外部 PSRAM | 大、慢、部分周邊不能 DMA | assets、點陣圖、不急的資料 |

⚠️ **S3 的 RMT DMA 不能指向 PSRAM** —— 燈效的輸出緩衝一定要 `NC_MEM_DMA`。

---

## 3. 燈效為什麼一定要 pool

**canonical 幀 = `px × 4 B`（R, G, B, W）。** 676 px = 2,704 B ≈ 2.6 KB。
（3 B/px 是 WS2812 的**線材格式**，在輸出階段才轉；canonical 一律 4 B，這樣同一塊緩衝可以餵
WS2812 / APA102 / PCA9685，換面板時上層不用改。）

管線需要同時存在：

| 角色 | 塊數 |
|---|---|
| compute 正在寫的（BACK） | 1 |
| 算完待送（READY） | 1 |
| output 正在送（FRONT） | 1 |
| 上一幀（LAST，逾時重播用） | 1 |

→ `nc_mem_pool_create("frame", NC_MEM_DMA, px*4, 4)`，之後每幀只是指標轉手，**一次配置都沒有**。
四個角色的狀態機見 [buffer-manager.md](buffer-manager.md) §6。

而 effect 的參數、mapping、mode 表屬於「reload 才換」的東西 → `arena`，`ASSET_RELOAD` 時整塊 reset，不用逐項 free（也就不會漏）。

---

## 4. 規則

- 熱路徑（frame / ISR / 每包）**不准**呼叫 `malloc` 或 `nc_mem_arena_alloc`。
- 一個用途一個 pool，不要讓兩個子系統搶同一個。
- `nc_mem_pool_get` 拿不到要**當成正常情況處理**（掉一幀），不是 assert。
- 開發期盯 `fails` 與 `peak`：`peak == count` 就是「差一點就掉幀」。
- arena 只在 reload 時 reset；**不要**在執行中 free 個別項目（arena 不支援）。

---

## 5. 現況（2026-10-31）

| 元件 | 狀態 |
|---|---|
| `nc_mem_pool_create / get / put / stats` | ✅ 已實作 |
| `nc_mem_arena_create / alloc / reset` | ✅ 已實作 |
| `nc_mem_cfg`（從 config.json 讀 Buffer 段、夾上限） | ✅ 已實作 |
| 開機煙霧測試 | ✅ main 取一塊再還回去，印 count/used/peak |
| 計劃／宣告／撇除／token | ❌ 未實作 → [buffer-manager.md](buffer-manager.md) |
| handoff / ring / queue / snapshot | ❌ 未實作 |
| lease / baseline verify | ❌ 未實作 |

### 已知問題（要修）

| # | 問題 | 後果 |
|---|---|---|
| 1 | `frame.block` 預設 **512 B** | 一幀 2,704 B，**這個 pool 連一幀都放不下** |
| 2 | `pool_get(pool, ms)` 內部 `vTaskDelay(1ms)` | 熱路徑用非 0 timeout → 掉幀變多毫秒卡頓 |
| 3 | 沒有 owner / generation | 忘 `put` → 「跑幾分鐘 pool 空了」，查不出是誰 |
| 4 | `Buffer.frame.block` 與 `System.num_pixels` 是兩套來源 | 改 px 不會改 block → 靜默壞掉 |
| 5 | `block_size` 只 4-byte 對齊 | DMA / cache line 建議 32 |
| 6 | cJSON 解析發生在 carve 之前 | 吃掉一點 `largest_free_block`（量小） |

---

## 6. 這些緩衝可以「燒死在 firmware」嗎

不行 —— 但也**不需要**那樣做。兩種做法的差別：

| 做法 | 配置能不能改 | 記憶體保證 |
|---|---|---|
| 靜態陣列燒進 firmware | ❌ 編譯期就固定 | ✅ 一定配得到 |
| **開機時依 config 向系統要**（採用） | ✅ 改 config 就好 | ⚠️ 要不到就報錯 |

DMA 也適用第二種：DMA 要的是「初始化之後位址固定」，不是「編譯期就要存在」。
開機配置、之後不動，就滿足 RMT / SPI 的要求。

**燒進 firmware 的是「預設值 + 上限」，config.json 決定「這次用多少」。**

```json
"Buffer": {
  "frame":  { "block": 512, "count": 4 },
  "assets": { "size": 65536 }
}
```

缺欄位 → 保留預設；超過上限 → 夾住並警告（`Buffer.frame.block` 上限 32 KB、count 上限 8、
assets 上限 512 KB）。壞 JSON → 整段用預設，不影響開機。

### 唯一做不到的事

**執行中改大小**。緩衝大小在開機配置完成後就固定，改了要重開機
（或走 `ASSET_RELOAD` 那條「重新載入」的路，但那是 arena 的 reset，不是 pool 的 resize）。

### 開機順序

```
storage_init()   /data 掛載、自動格式化、寫預置、設卷標
      ↓
讀 /data/config.json → 覆寫 buffer 參數
      ↓
建立 frame pool / asset arena
      ↓
USB 起來、碟交給 host
```

config 從 `/data` 讀，所以**必須**在 storage 之後、USB 之前 —— 這個順序不能顛倒。

---

## 8. Plan：宣告 → 一次配置 → 查詢

`nc_plan` 是「系統級」的那一層：它不認得任何子系統，只認得**宣告**。

```c
nc_plan_declare(&(nc_plan_decl_t){
    .name = "pixel.frame", .group = "pixel", .level = 2,
    .mech = NC_MECH_HANDOFF, .cls = NC_PLAN_DMA,
    .bytes = 2704 * 4,
});
nc_plan_commit(avail, allow_shed);
void *mem = nc_plan_get("pixel.frame");
```

### 三條規則

| 規則 | 行為 |
|---|---|
| 宣告窗口在 commit 關閉 | 之後再宣告回 `ESP_ERR_INVALID_STATE`（不是 assert） |
| 撇除以 group 為單位 | 按 level 大到小、同 level 按 group 名排序 → **與宣告順序無關** |
| level 0 永不撇除 | 撇完仍不夠 → `ESP_ERR_NO_MEM`，不靜默啟動 |

`allow_shed` 是 policy：開發期 `false`（大聲拒絕），量產可 `true`。

### 查詢指令（重用既有的 STATUS 域）

```bash
tools/nc4.py --port ... raw 0x1101 <str>   # keys：空=全部、?=目錄、mem / sys / task
```

`keys="mem"` 回 plan + buf；`keys="?"` 列出可用名稱。**沒有新增指令碼** ——
0x1101 / 0x1102 是 `slave/schema/status.json` 就有的。

```json
{"plan":{"committed":true,"decl":2,"shed":0,
         "bytes":{"dma":2048,"int":0,"psram":65536},
         "items":[{"n":"pixel.frame","g":"pixel","lvl":2,"b":2048,"shed":false},
                  {"n":"assets","g":"assets","lvl":3,"b":65536,"shed":false}]},
 "buf":[{"n":"probe","blk":256,"depth":8,"used":0,"peak":2,"drops":0,"starve":0}]}
```

### 這個查詢立刻抓到一個 bug

上面 `pixel.frame` 的 `b` 是 **2048** —— 也就是 4 槽 × **512 B**。
一幀 676 px × 4（RGBW）是 **2704 B**，這個 pool 連一幀都放不下。
（`Buffer.frame.block` 預設 512 是舊值，該由 `System.num_pixels` 推導。）

**看得見才修得掉** —— 這就是查詢指令存在的理由，不是為了好看。

### Host 測試

`tools/hosttest/plan_test/run.sh`（gcc，零依賴、不需要板子）：6 個場景，含容量充足、
超預算拒絕、超預算撇除、決定性（倒序宣告結果相同）、窗口關閉、重名。
