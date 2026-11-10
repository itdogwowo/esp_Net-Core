# 契約總表（誰負責什麼、邊界在哪）

> 2026-11-10。**用途**：所有「不變的約定」集中在這一頁，其他文件只指向它，不再各自寫一份。
> 細節來源：[buffers.md §9](buffers.md)（C1–C11）、[execution-rules §1.3](execution-rules.md)（優先級）、
> [task-lifecycle.md](task-lifecycle.md)（生命週期實作）、[glossary.md](glossary.md)（字眼）。
>
> 讀法：每一節有 **「誰 owns」→「邊界」→「違反會怎樣」**。☆ = 今日已實作；★ = 規範已定、未實作。

---

## 1. 任務管理器 ↔ 任務

### 1.1 誰 owns 什麼

| 東西 | 誰 | 邊界 |
|---|---|---|
| 登記、排序、開／停 loop | **管理器** | 任務只回傳 `const nc_task_t *`，**唔准** `xTaskCreate` |
| 資源（緩衝區）建立 | **管理器** | 任務只 `declare()`；`on_start` 只收指標（§2） |
| 指令（action） | **解碼器** | 任務**唔擁有**指令；指令由 action 模組註冊（§4） |
| 硬體 driver | **任務**（owner） | 一個裝置／一條 bus 只有一個 owner；其他人經它的 `provides[]` |
| 接線宣告（`claims[]`） | 任務宣告、管理器審 | 只擋重複宣告，**唔負責序列化**（序列化靠「一個 owner」） |
| **優先級** | **管理器** | 非 SYSTEM 任務唔准入保留區 7–15；寫錯會被夾到 6 並印警告（見 §3） |
| 量測（loop 時間） | **冇人** | 2026-11-06 起管理器唔再量（見 task-lifecycle） |

### 1.2 硬上限（超過就拒絕或截斷，唔會靜默長大）

| 常數 | 值 | 意思 |
|---|---:|---|
| `NC_TASK_MAX` | **20** | 管理器最多幾個任務（固定 8 ＋ `.fxe` 最多 8 ＋ 4 備用） |
| `NC_TASK_MEM_MAX` | 8 | 一個任務最多幾項資源（`mem[]` ＋ `declare()` 合計） |
| `NC_TASK_LOOPS_MAX` | 2 | 一個任務最多幾條線程（`nc4` 兩核各一） |
| `NC_TASK_LAYERS` | 8 | loop 的 layer 只有 0–7 |
| `NC_PLAN_MAX_DECLS` | **16** | 全系統宣告項數（**更正：舊文件寫 64**） |
| `NC_PROVIDE_MAX` | 24 | 全系統 provide 條數 |
| `NC_BUF_MAX` | **32** | 全系統緩衝區條數 |

### 1.3 兩個次序（唔可以撈亂）

| | `level`（任務） | `layer`（loop） |
|---|---|---|
| 管什麼 | 幾時**宣告資源**、幾時 `on_start` | 幾時**開線程** |
| 範圍 | 0 以上；`-1` = 完全唔啟動 | 0–7 |
| 默認關係 | 通常相等；唔相等要寫註解講原因 | |

### 1.4 config 管租戶，唔管房東

- `SYSTEM`（nc4／net／…）：**唔讀** `tasks.json`，照 C 宣告跑。保證「就算 config 全錯都開得機」。
- `NORMAL`／`EFFECT`：`level`／`affinity` 由 `tasks.json` 決定；未設定 = `[0,0]` = 唔開 loop。
- 補寫規則：固件有、config 冇 → 開機加一條 `[0,0]`（只加不改）。

### 1.5 失敗語義：全有或全無

- 資源**逐項**建立；一項要不到 → 其他照建立，但**整個任務不啟動**，`on_start` 唔會被呼叫。
- 已建立的項**唔會退回**（冇 free）→ 那塊記憶體永久屬於那條 buffer。
- 判斷方式：開機 log ＋ `STATUS keys=mem`。

### 1.6 attach／detach（只限 `EFFECT`）

| 限制 | 理由 |
|---|---|
| 唔可以帶 `mem[]` | 宣告窗口在 commit 已關；runtime 記憶體只可以由父層 arena `carve` |
| 唔可以係 `SYSTEM` | 系統層由韌體決定，唔係由可替換的檔案決定 |
| 指令已被別人註冊 → 整個 attach 失敗 | 唔要半套 |
| loop 停唔下來 → detach 失敗 | 指標仲活住就拆 = use-after-free |

### 1.7 停滯偵測（心跳）

| 約定 | 說明 |
|---|---|
| 管理器提供一個 4 B 遞增計數（每 loop 一個） | **唔開計時器、唔開任務**；舊 `monitor`／`epoch` 已刪 |
| 短 fn（會返回） | 管理器每輪自動 `++` |
| **長跑 fn（`for(;;)`，例如 `decode0/1`、`fx`）** | **必須自己叫 `nc_task_beat()`**，否則永遠停在 1 |
| 停滯判斷 | **主機**比對兩次 `STATUS keys=tasks` 的 `b` 差值；差值 0 ＝ 停滯 |

實測（2026-11-10，S3）：`decode0/1` 閒置 21/20 次每秒（受 50 ms 安全網限制）、
`fx` **100 次每秒**（10 ms 週期）、`hello` 1 次每秒（1 s 週期）、`bench_*` 0（`[0,0]` 未啟用）。

### 1.8 違反會怎樣

| 違反 | 後果 |
|---|---|
| 任務自己 `xTaskCreate` | 冇人管它的棧／停止／狀態；`STATUS keys=tasks` 見唔到它 |
| 重名 | `register` 回 `ESP_ERR_INVALID_STATE` |
| 超過 20 個 | 回 `ESP_ERR_NO_MEM`（**唔會**置換舊的） |
| `claims[]` 衝突 | 帳本擋後者（按登記次序，先到先得）；`task_test` 有覆蓋 |

---

## 2. 任務 ↔ 緩衝區（摘要；全文 C1–C11 在 [buffers.md §9](buffers.md)）

**一句話**：*任務只宣告、管理器建立、`on_start` 之後只有一條指標；停止唔會消失，
要給下一個用戶就 `handover`。*

| # | 約定 |
|---|---|
| C1 | 宣告 = `name`（`<任務>.<用途>`，全系統唯一）＋ `where`（S／P／DMA）＋ `slot_bytes × n` |
| C2 | 管理器逐項建立；一項失敗 → 整個任務不啟動 |
| C3 | `on_start(res, n)` 是**唯一**拿到緩衝區的地方；宣告 `slot_bytes × n` 的項直接是 `res[i].buf`（P5 完成，2026-11-10） |
| C4 | 停止之後**仍然存在**；重啟重用同一批指標（`on_start` 只跑一次） |
| C5 | 只有 owner 可以 `handover`；換手前所有格要 FREE；記憶體唔搬 |
| C6 | FIFO＝一寫一讀；多 writer 自己用 mutex 序列化（工具只計 `viol`） |
| C7 | 熱路徑只准 put/get；`create`／`handover` 只可以開機或換場 |
| C8 | 帳以 `mem.buf` 為準；對外只經 `provides[]`，唔好傳 `nc_buf_t *` |
| C9 | runtime 任務唔可以宣告；只可以由父層 arena 切 |
| C10 | `NC_BUF_MAX` 32、滿了回 NULL ＋ log |
| C11 | 每個用法要對得上 [buffer-patterns.md](buffer-patterns.md) 的一個模式 |

**唯一的「釋放」例外** ★：`NC_BUF_F_RUNTIME`（執行期建立，例如 parser 隨通道開關）
可以用 `nc_buf_release()` 還返；**開機建立的一律拒絕**（R2：冇 free 就冇碎片）。

---

## 3. 優先級保留區

**NC 核心（收 → 解 → 回）獨佔 7–15**；其他任務一律 ≤ 6。常數：[nc_prio.h](../../firmware/components/sys/task/include/nc_prio.h)。

| 優先級 | 誰 |
|---:|---|
| 24 / 22 | `ipc1` / `esp_timer`（IDF） |
| **15–7 保留** | `decode0/1` = **8**、bus producer（`cdc.rx`／`uart.rx`）= **7** |
| 6 | 其他任務上限 |
| 5 | `TinyUSB`（IDF） |
| 2 | `fx` / `nc_log` |
| 1 | `app_main` / `hello` / bench |

| 約定 | 實測依據（2026-11-10，S3） |
|---|---|
| **decode 必須高過同核 producer** | 反過來：push→decode 接手 478 µs、300 幀 workload `slot_full` +122；倒過來 42 µs、`slot_full` 0 |
| **新任務唔可以入 7–15** | 要更快就提出來討論，唔可以自己搶 |
| producer 由 `nc4_bus_start_producer()` 自動夾到 `decode-1` 並警告 | 唔應該靠它；寫錯唔會靜默死鎖，但會慢 |

---

## 4. 解碼器 ↔ 動作

| 約定 | 說明 |
|---|---|
| **一個 decoder 一次只跑一個 handler** | `decoder_lock()`；每個核一個 decoder（回覆幀 per core） |
| **handler 內唔可以再 `nc4_parser_feed()` 同一個 decoder** | FreeRTOS mutex **唔係遞迴** → 自我死鎖，整個 decoder 停住，WDT 救唔到（2026-11-10 實際撞到） |
| 要跑第二條解碼路徑 | 一定要**另一個 decoder** 或**另一個任務**（見 status §7.38） |
| 指令唔屬任何任務 | action 模組提供 `<domain>_register(dec)`，開機由 `actions_init()` 一次註冊 |
| `frame.data` 只在 handler 期間有效 | 零複製借用；要留就自己 copy |
| 轉寄先於本地執行 | `parser_forward()` 在 ADDR 過濾與 handler 之前（frame 一返回就會失效） |

---

## 5. 通道 ↔ 核心 ↔ 路由

| 約定 | 說明 |
|---|---|
| 一條 link 一個 owner 核 | 由 `routes[].out` 的 `decodeN` 推導；冇人指向 → 該 link 唔建立 |
| parser 狀態 per-link | 半幀位置、重同步計數永遠唔共用 |
| 跨核只傳**資料**，唔傳**喚醒** | 唯一例外：轉寄的 `nc4_bus_kick_core`（同核 producer 的 kick 唔算跨核） |
| PUSH 模式 | producer 讀 driver → 寫 slots → 同核 kick；producer 必須低過 decode（§3） |
| PULL 模式 | decode 自己讀 driver（會用 10 ms 輪詢）；只在 driver 非阻塞時用 |
| `routes[].out` | `decodeN` = 本地執行；link 名 = 轉寄；空 = 丟棄；兩者一齊寫 = 兩樣都做 |
| 轉寄失敗只可以丟 | 來源 buffer 在 handler 返回後失效，冇得重試；由 link 表計 `drops` |

---

## 6. 初次開機與配置生命週期（☆ 已實作）

| 情況 | 行為 |
|---|---|
| **第一次開機**（NVS 冇 `PROVISIONED`） | 允許格式化 `/data`；格式化後 `seed_presets()` 寫入預置；成功才標記 `PROVISIONED=1` |
| 已經初始化過、掛載失敗 | **唔會**自動抹分區（保護使用者資料）→ 由 host 修復，或 `STATUS_SET fs_format=1` ＋ REBOOT |
| **換韌體**（`fw_hash` 變了） | 記 `FORMAT_PENDING` → 下次開機**格式化重建** ＋ 重新 seed（唔係逐個刪 config 檔） |
| 預置檔案 | `seed_presets()` **只補缺檔**，已有的檔一律唔動 |
| 明確重建 | `STATUS_SET System.fs_format=1` ＋ REBOOT（只在開機路徑消費，唔會在解碼路徑抹 7 MB） |
| 判斷依據 | NVS 記錄：`PROVISIONED`／`FORMAT_PENDING`／`fw_hash`／`degraded` |

**注意**：換韌體會清掉用戶改過的 `/data`（config／效果檔）—— 這是刻意設計（舊 config 蓋住新預設）。
要保留就把檔案放喺 `/data` 之外（未做，見 §8）。

---

## 7. 現在有什麼（盤點）

| 區域 | 已有 | 部分／缺口 |
|---|---|---|
| 任務管理器 | 五階段、level/layer、config 補寫、attach/detach、provides、帳本 | `claims[]` 的 `audit()` 有 bug；冇 loop 心跳（stall 偵測） |
| 緩衝區 `nc_buf` | FIFO（唯一格模式）＋ STREAM、`off`·`len`·`gen` 32-bit、`handover`、runtime release、`mem.buf`、`xcopy`、`abort`；**parser 環、回覆幀、bus 插槽環、跨核轉寄全部在帳上** | `carve` 未做；`nc_hub` 只剩測試用戶（P5）；TX 環由 driver 擁有（唔搬） |
| 優先級 | 保留區 7–15、decode>producer、自動夾 | 冇 CI／開機檢查阻止新任務入保留區 |
| 解碼器 | 每核一個、dispatch mutex、9 個 framing case 全綠 | — |
| 路由 | `system.json` 表、`0x1105` 查表、轉寄出口、跨核 fwd queue | 環狀拓撲未擋；forward-only route 未支援 |
| 通道 | CDC（PUSH，實測 kick 42 µs）、UART（PUSH，driver 未開）、ESP-NOW（測試版） | TCP／WS／UDP 只在 P4；UART 等腳位 |
| 初次開機 | 完整（provisioned／fmt_pending／fw_hash／degraded） | 冇「保留 user_data」機制 |
| `.fxe` | 掃描、驗證、manifest、attach | WAMR 執行引擎未接（`loops=[]`） |

---

## 8. 待你決定（我先列出，唔自己拍板）

1. **`claims[]` 的 audit bug**：要修，還是先擱置（只有 pin/bus 宣告用到）？
2. **loop 心跳（stall 偵測）**：要不要每 loop 一個 `_Atomic beats`（4 B）＋ 主機比對？
3. **換韌體清 `/data`**：要唔要一個「user 專區」唔會被清？
4. **保留區的強制**：只寫文件，還是在 `nc_task_boot()` 加警告／拒絕？
5. **`nc_buf_carve()`**：P5 前要不要先做（`.fxe` 現在用自己的 arena）？
6. **UART 腳位**：SD 佔用 GPIO7/8，要換腳還是等新板？
