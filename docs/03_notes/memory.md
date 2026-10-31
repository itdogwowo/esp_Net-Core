# 記憶體管理

> 原則：**熱路徑不 malloc。** 會隨時間反覆發生的東西（幀、封包、DMA 緩衝）用 pool；
> 一次配、一次放的東西（assets、config、解析結果）用 arena。

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

一幀 = `px × 3 B`。676 px ≈ 2 KB。管線需要同時存在：

| 用途 | 塊數 |
|---|---|
| compute 正在寫的 N+1 | 1 |
| output 正在送的 N | 1 |
| 上一幀（重播用） | 1 |
| 餘裕 | 1 |

→ `nc_mem_pool_create("frame", NC_MEM_DMA, 676*3, 4)`，之後每幀只是 `get` / `put`，**一次配置都沒有**。

而 effect 的參數、mapping、mode 表屬於「reload 才換」的東西 → `arena`，`ASSET_RELOAD` 時整塊 reset，不用逐項 free（也就不會漏）。

---

## 4. 規則

- 熱路徑（frame / ISR / 每包）**不准**呼叫 `malloc` 或 `nc_mem_arena_alloc`。
- 一個用途一個 pool，不要讓兩個子系統搶同一個。
- `nc_mem_pool_get` 拿不到要**當成正常情況處理**（掉一幀），不是 assert。
- 開發期盯 `fails` 與 `peak`：`peak == count` 就是「差一點就掉幀」。
- arena 只在 reload 時 reset；**不要**在執行中 free 個別項目（arena 不支援）。

---

## 5. 現況

| 元件 | 狀態 |
|---|---|
| `nc_mem_pool_create / get / put / stats` | ✅ 已實作 |
| `nc_mem_arena_create / alloc / reset` | ✅ 已實作 |
| 開機煙霧測試 | ✅ main 取一塊再還回去，印 count/used/peak |
| 幀 pool 的實際尺寸 | ⏳ 等每節點像素數拍板 |

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
