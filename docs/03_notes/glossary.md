# 字彙表（術語一句話）

> 2026-11-10。**用途**：這個項目用到的字，每個只留一個意思。
> 原則：**code 用英文、文件用中文，一對一**；同一個字不准指兩樣東西。
> 相關：[buffers.md](buffers.md)（緩衝工具本體）、[task-model.md](task-model.md)（任務模型）、
> [naming.md](naming.md) §5（FXE 相關術語）。

---

## 1. 執行：任務 vs 線程（最常撞的一組）

| 詞（code） | 一句話 | 在哪 |
|---|---|---|
| **任務** `nc_task_t` | 一個**登記單位**：身份 ＋ 要幾多記憶 ＋ 生命週期回呼 ＋ 0–2 條線程 | `sys/task/include/nc_task.h` |
| **線程／迴圈** `nc_task_loop_t` | 真正被 FreeRTOS 調度的東西（有棧、優先級、綁核） | 同上 `loops[]` |
| **管理器** | 登記 → 宣告 → 啟動 → 開線程 → 停止；任務只交宣告，不碰 `xTaskCreate` | `sys/task_manager/` |

> ⚠️ **「任務」不是線程。** 一個任務可以 0 條線程（`level:-1` 或 `affinity:[0,0]`），
> 也可以 2 條（`nc4` 的 `decode0`／`decode1`）。

| 詞 | 意思 | 易誤會位 |
|---|---|---|
| `kind` | `SYSTEM`（韌體一部分、config 管唔到、唔卸載）／`NORMAL`／`EFFECT`（`.fxe`，可黏可拆） | 唔是「類型」，是「邊個有權管它」 |
| `level` | **任務**的次序（細先）：幾時宣告資源、幾時啟動 | 同 `layer` 成對 |
| `layer` | **線程**的次序（細先）：幾時開跑 | 通常等於 level；唔等要寫註解 |
| `affinity` | `[core0, core1]`，只准一個 `1` | 唔是效能調優，是「釘死」 |
| `enabled()` | 房東（固件）宣告的開關：回 false 連棧都唔建立 | config 改唔到它 |

**三種「唔跑」**：config `level:-1`、config `affinity:[0,0]`、房東 `enabled()`。
前兩個管租戶、第三個管房東（硬體存在性），不可互換。

---

## 2. 記憶體

| 詞（code） | 一句話 | 狀態 |
|---|---|---|
| **緩衝區** `nc_buf` | N 個等長**格** ＋ 每格一個狀態 | **唯一工具**（[buffers.md](buffers.md)） |
| **格** `slot` | 一格資料，寫得滿、讀得空 | — |
| **arena** | 開機早期一次拿的一大塊記憶體，`nc_buf` 由它切 | plan 的新身份 |
| `S`／`P`／`DMA` | internal SRAM／PSRAM／可 DMA 的 internal | 三個 class |
| **FIFO** | 格的取用順序（嚴格先入先出）——**唯一**的格模式；冇「隨機」 | 一個參數，不是型別 |
| `S2P`／`P2S`／`S2S`／`P2P` | 「格在哪 × 資料由哪來」的四種組合（同一個 `put()`） | 不是四種 buffer |

**退役的詞**（文件與程式都不要再新增）：

| 退役 | 改用 | 為什麼 |
|---|---|---|
| `hub`（`nc_hub`） | **FIFO 緩衝區** | 借來的名：網絡的 hub = 集線器／廣播，而它只交畀一個讀者 |

| `環`（兩種意思） | 「TX 環」寫 `tx` buffer；hub 寫 FIFO buffer | UART RX 環與 hub 都叫「環」，撞名 |
| `plan`（`nc_plan`） | **arena**（`nc_buf` 的內部機制） | 它是分配策略，不是緩衝區 |
| 三階段 `declare→commit→take` | `create`／`carve` | 三個動詞對應兩個動作，多餘 |

---

## 3. 通訊

| 詞 | 一句話 |
|---|---|
| **通道** `link` | 一條實際通訊線：CDC／UART／TCP／WS／UDP／SPI／ESP-NOW |
| **路由** `route` | 邊條線入 → 邊個核解（`decodeN`）→ 有咩出口（`out[]`） |
| **解碼器** `decoder` | 每核一個：收 → 驗 CRC → 查表 → 派。**不是硬體解碼器** |
| **動作** `action` | 一條指令做的事（handler）。**不屬於任何任務** |
| **parser** | 每條線自己一份「湊幀狀態」（半幀、重同步）。狀態永遠 per-link，不共用 |
| **轉發** `fwd` | 收到之後唔解，原幀交去另一條線 |
| **回覆幀** `reply` | 每個 decoder 用來打包回覆的緩衝（2,615 B，`P`） |

---

## 4. 生命週期與接線

`register`（登記）→ `declare`（講要幾多）→ `commit`（一次要）→ `start`（資源到手、`on_start`）
→ `boot`（開線程）→ `stop`（合作式停）。`attach`／`detach` 只有 `.fxe` 用。

| 詞 | 意思 |
|---|---|
| **契約** | 任務同緩衝區之間不變的約定（C1–C11，[buffers.md §9](buffers.md)） |
| **宣告窗口** | `declare` 之後、啟動之前；關了之後只可以在 arena 內 `carve` |
| **owner** | 一條緩衝區的擁有者；只有它可以 `handover` |
| `claim` | 我要用某個硬體資源（帳本擋重複宣告） |
| `provide` | 我給別人什麼（buffer／JSON／單一數值，取用一律經 getter） |
| `stall` | 線程停滯。**現時冇機制偵測**（`monitor` 已刪）；要不要加心跳未定，見 [buffers.md §9](buffers.md) |

---

## 5. 一句話總結

**開機時，任務向 arena 要 buffer；線程由 buffer 拿格；格裡的資料按 S／P 規則搬；
所有帳在 `mem.buf`。**

字少了，但每個字都只有一個意思 —— 這就是這個字彙表的目的。
