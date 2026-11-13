# 緩衝用法模式（patterns）

> 2026-11-10。**性質：概念與用法，程式未實作。**
> 這頁只講「**怎麼用**」；工具本身（`nc_buf`）見 [buffers.md](buffers.md)。
> 標記：✅ 已實作　🧪 已量測但未實作　❓ 未量（假設，實作前要驗）

---

## 0. 三條判準：什麼進工具、什麼寫成模式

| 情況 | 去邊 |
|---|---|
| 只係參數唔同（格大小、格數、位置、reserve） | **參數** |
| 需要新的「格狀態」或新的「交棒語義」 | 工具（慎重） |
| 只係某個任務的資料格式、時序、輪替方式 | **模式（本頁）** |
| 需要工具提供新的系統呼叫 | 工具 |

**一句話**：如果一個新用途令工具要多一個 mode，先問「能不能用現有的
`put`／`get`／`off`·`len`／換手表達」。可以 → 寫成模式；唔可以 → 才改工具。

**怎麼驗**（2026-11-10 起）：

| 層 | 工具 | 跑什麼 |
|---|---|---|
| 主機（無板） | `bash tools/hosttest/buf_test/run.sh` | 9 組：狀態機／FIFO／reserve／32-bit／S·P 記帳／換手／registry 滿／JSON 截斷 |
| 上板 | `python tools/buf_test.py --port COM8 --status` | `0xF032`：對齊／FIFO／滿了回 NULL／reserve／四條 copy 路徑／換手，再印 `mem.buf` |

**為什麼要分**：工具的價值在「所有緩衝同一個帳、同一個生命週期規則」；
用法的價值在「每個場景的時序與對齊」。混在一起，就會重演「plan／pool／hub／環
四個名指同一類東西」那種情況。

---

## 1. 模式總表

| # | 模式 | 位置 | 格大小 × N | 誰寫 → 誰讀 | 生命週期 | 狀態 |
|---|---|---|---:|---|---|---|
| P1 | 位元組流水（RX parser） | S | **STREAM** 1–4 KB | 驅動 → 解碼核 | 常駐 | ✅ 工具已支援 |
| P2 | 命令 staging | S | 1,024 × 8 | 解碼核 → 任務 | 常駐 | ✅（預設開） |
| P3 | 回覆打包 | **P** | 2,615 × 每核 1 | 解碼核 → transport | 常駐 | ✅ |
| P4 | TX 環 | S | 依 transport | 任務 → 驅動 | 常駐 | ✅ |
| P5 | 跨核轉寄 | **P** | 4,160 × 2 × 16 | 來源核 → 目的核 | 常駐 | 🧪 設計已定 |
| P6 | 大頁雙／三緩衝（LED） | **P** | 38,400 × 2–3 | 渲染 → DMA | 常駐 | 🧪 |
| P7 | DMA 波形（I80） | **S** | 28,800 × 2 | 編碼器 → DMA | 常駐 | 🧪 |
| P8 | 大頁分塊（LCD multi-pass） | P → S | 幀 W×H×2；tile 16 KB | 渲染 → tile → DMA | 常駐 | ❓ |
| P9 | 階段 arena（`.fxe` 換場） | **P** | 512 KB | 引擎 → 模組 | **階段**（換手） | ✅ |
| P10 | OTA／tile 輪替 | S | 4,160 × 3 | OTA → flash | **階段**（換手） | ❓ |

---

## 2. 逐個模式

### P1 位元組流水（RX parser）

**用途**：一條通道收到半幀、要重同步時，逐位元組推進。

- 形狀：`where=S`、`NC_BUF_F_STREAM`；CDC 4,128／chunk 1,024／budget 8；UART 1,024／chunk 256。
- 流程：`nc_buf_write`（驅動寫入，可部分）→ 解碼迴圈 `nc_buf_read_ptr` 拿到**一段連續**
  → parser 推進 → `nc_buf_skip(total)`；壞幀只 `skip(1)` 重同步。
- 陷阱：
  1. **parser 狀態永遠 per-link**，唔可以共用（半幀位置係每條線自己的）。
  2. 一格只可以有一個 writer；兩個核同時餵同一條環 = 直接壞。
  3. 呢個模式**唔需要格**，需要的是「連續位元組」——所以它是流水，不是格。

### P2 命令 staging

**用途**：小命令（<1 KB）臨時放一下，跨任務交棒。

- 形狀：S／1,024 × 8／常駐但**可以關**（`system.json`）。
- 流程：`put(src,len)` → 任務 `get` → `get_end`。
- 陷阱：開著就白佔 8 KB internal；現時冇 header-first assembler 接上，**預設應該關**。

### P3 回覆打包

**用途**：handler 把 JSON 打包成回覆幀（2,615 B），交給 transport。

- 形狀：**P**／2,615 × **每核 1 格**／常駐。
- 流程：`put_begin` → 寫 JSON → `put_end(len)` → transport 讀。
- 陷阱：
  1. 一定要**每核一格**：兩個解碼核的 handler 是可以同時跑的（§7.26），共用一格就會互相蓋掉。
  2. 現時是 `heap_caps_malloc`，`keys=mem` 睇唔到 → 統一之後帳面要出現。
  3. `len` 可能遠細過 2,615，`get` 之後**只可以用 `len` 範圍**。

### P4 TX 環（transport 擁有）

**用途**：交畀 UART／CDC／TCP 之前的最後一段。

- 形狀：S／UART 1,024／常駐。
- 行為差異（實測）：CDC 4 KB FIFO **不用等**；UART 1 KB 環會**排空**（4 KB 轉寄阻塞 ~44 ms @921600）；TCP 等到 ACK。
- 陷阱：
  1. **唔可以由 buf 工具去等**：等係 transport 的行為，工具只回「滿了」。
  2. 大幀唔好塞細環：4 KB 入 1 KB 環 = 拆成 4 次寫，中間可能有半幀在環上。

### P5 跨核轉寄（唯一需要跨核的模式）

**用途**：收到之後唔解，原幀交給另一條線（可能在另一個核）。

- 形狀：**P**／4,160 × 2 × 16 條（per 目的 link × 來源核）／常駐。
- 流程：來源核 `put` → 目的核每輪 `get` 最多 **4 幀** → `nc4_link_send` → `get_end`。
- 陷阱：
  1. **嚴格 FIFO**，唔可以跳格（N=2 時跳格會 gap／dup，2026-11-09 壓力測試已證）。
  2. 慢 UART 只可以阻塞**目的核**，唔可以阻塞來源核 —— 這正是它要獨立成一條 buf 的理由。
  3. 跨來源順序**不保證**（兩條 hub → 同一個 link）。
  4. `len` 一定要用：今日 `nc_hub` 冇 `len`，所以只可以送滿格（見 §3 第 1 條）。

### P6 大頁雙／三緩衝（LED）

**用途**：渲染寫一頁、DMA 讀另一頁。

- 形狀：**P**／38,400 × 2（雙緩衝 75 KB）或 × 3（115 KB）／常駐。
- 流程：渲染 `put_begin`（reserve 16 B 放 frame id）→ 寫滿一格 → `put_end(len)` → DMA 完成 callback 才 `get_end`。
- 數字（實測／推導）：16 lane × 600 顆 × 4 B（GRBW）= 38,400 B；GRBW 40 µs/LED → **24 ms/幀 = 41.7 FPS**；只送 24-bit（3 B/LED）→ 18 ms = 55.5 FPS。
- 陷阱：
  1. **DMA 完成才可以還格**，否則 torn frame。
  2. **buffer 用量 ≠ wire 用量**：4 B/LED vs 3 B/LED，混了就會「DMA 讀到未寫的範圍」。
  3. `off` 要 64 B 對齊（cache line）；reserve 的表頭唔可以令資料起點唔對齊。
  4. PSRAM 上寫一幀要 flush：msync **0.4–0.5 ms**，要計入 20 ms 預算。

### P7 DMA 波形（I80）

**用途**：I80 送出 LED 波形（不是像素）。

- 形狀：**S**（一定）／28,800 × 2／常駐。
- 數字（`0xF030` 實測）：submit **70–122 µs**、wire **1.9–7.3 ms**（pclk 2–8 MHz）、切片 fill **62–64 µs** 完全被 DMA 藏起。
- 陷阱：
  1. **S3 的 I80 DMA 只可以讀 internal**（已量）→ 波形 buffer 唔可以放 PSRAM。
  2. GPIO **33–37 是 N16R8 octal PSRAM**，16-lane 排線一定要避開（第一次用 33–48 一送就 reset）。
  3. 一半（P6 的幀）在 P、一半（P7 的波形）在 S → 每次換頁都要 P→S copy（~1.2 ms／38.4 KB）。

### P8 大頁分塊（LCD multi-pass）

**用途**：一幀大過 cache，要多次 pass 才能算完。

- 形狀：幀在 **P**（`W × H × 2`，RGB565）；工作區是 **S tile ≤16 KB**。
- 流程：pass 1 從 P 讀一塊入 S tile → S 內做完所有 pass → **一次**寫回 P → DMA 讀 P（或再 P→S）。
- 數字：PSRAM copy 20–33 MB/s；兩核同時 copy 腰斬到 **10 MB/s**；tile 只快 1.3×（16 vs 12 MB/s），價值是 **bound latency**。
- 陷阱：
  1. **唔要每個 pass 都掃 PSRAM** —— 2–3 個全幀 pass 就把預算食光（38.4 KB 一 pass 1.2 ms）。
  2. 大幀 copy 時間 ≈ `bytes ÷ 20 MB/s`：300 KB ≈ 15 ms、750 KB ≈ **37 ms**（已超一個 frame 預算）。
  3. flash 寫入時 cache 停 → PSRAM 一齊停，LCD 會凍結一格。

### P9 階段 arena（`.fxe` 換場）

**用途**：effect 模組換場時，舊模組的記憶體還回去給新模組。

- 形狀：**P**／512 KB（`fx.arena`）／**階段**。
- 流程：換場 → 檢查舊模組全部格 IDLE → `handover` 給新模組（**記憶體一動不動**）→ `generation++`。
- 陷阱：
  1. 舊模組的把手一用就要被拒（generation 對不上）。
  2. 換場期間唔可以仲有 DMA／transport 揸住舊模組的格。
  3. 只准**整塊**換手，唔准切細（見 [buffers.md](buffers.md)）。

### P10 OTA／tile 輪替

**用途**：OTA 寫 flash 的 staging 與渲染的 tile 唔會同時用，可以輪流。

- 形狀：S／4,160 × 3（OTA）↔ S／16 KB（tile）。
- 做法：同一塊 S arena，兩個 owner 輪流；唔用時 `handover`。
- 陷阱：**flash 寫入會停 cache → PSRAM 也停**，所以輪替期間唔可以有人在用 PSRAM 做實時工作。

---

## 3. 小技巧集（實作前一定要記得）

1. **`len` 一定要 32-bit**。今日 `nc_pool` 是 `uint16`（上限 65,535）：LED 38,400 剛好過，LCD 750 KB 直接爆。
2. **`gen` 一定要 32-bit**。今日是 `uint16`：5,000 幀／秒的話 **13 秒就 wrap**，防 ABA 形同虛設。
3. **每格都要有 `len`**。`nc_hub` 今日冇（`write_commit` 冇 length 參數）→ 只可以送滿格，一有「唔齊整」就無解。
4. **`off` 用來對齊**：資料起點要 64 B（cache line）／32 B（DMA）。reserve 的表頭要令 `off + reserve` 仍然對齊。
5. **表頭放格內前綴（reserve），工具欄位放 descriptor**。前者是「你的語意」，後者是「範圍與狀態」，兩者唔可以互相侵入。
6. **`cap` 是 buffer 屬性，`len` 是每格屬性**。唔好把「格幾大」同「用幾多」混成一欄。
7. **buffer 用量 ≠ wire 用量**（4 B/LED vs 3 B/LED）。工具只記 buffer 用量。
8. **DMA 完成才可以還格**。提前還 = torn frame。
9. **DMA 只讀 S**（S3 I80 已量）。P 上的大頁每次要 P→S copy。
10. **PSRAM 頻寬是全核共用**：兩核同時 copy 由 20–21 掉到 10 MB/s。大 copy 集中一個核、一個時段。
11. **多 pass 要喺 S tile 做**，唔好每 pass 掃 PSRAM；多 pass 之後**一次**寫回 P。
12. **寫完要 flush**：38.4 KB 的 dirty 區 msync 約 0.4–0.5 ms，而且**唔同大小差唔多**（cache 只有 32 KB）。
13. **flash 停 cache → PSRAM 一齊停**。PSRAM 上唔好放有實時要求的東西。
14. **跨核只有轉寄一個地方**。其餘全部「同核、單 owner、即時消費」，唔需要序號、唔需要跨核喚醒。
15. **零複製只在「借看」期間有效**：handler 拿到的指標，函式返回後就失效；要留就自己 copy 走。
16. **唔好在熱路徑做建立／換手**。`create`／`handover` 只可以開機或換場時做。
17. **零複製有三條路**：① 寫入目的地（`put_begin`）；② 借用（`get`，指標本來就係同一塊）；
    ③ 整條換手（`handover`）。只有 `put(src)` 係複製。
    `xcopy > 0` = 你抄了「自己另一條 buffer」，通常可以改成 ① 或 ②。

---

## 4. 唔好濫用模式的訊號

- 一個模式需要「工具多一個 mode 才寫得出來」→ 先質疑模式設計，唔係先改工具。
- 一個 buffer 有**兩個 writer 或兩個 reader** → 唔係模式問題，是設計錯誤（除非明確用單寫單讀拆成兩條）。
- 一個 buffer 同時想「整格原子」同「位元組部分消費」→ 拆成兩條 buf（原子用格、流水用環），唔好一條做兩樣。
- 為了省記憶體而把兩條生命週期重疊的 buf 併成一條 → **錯**（例如 LED 幀與 fx arena，效果輸出就是 LED，一定重疊）。

---

## 5. 未定（實作前要答）

1. P4 的「滿了」要不要提供等待（現時一律回 NULL）。
2. P5 的每輪抽幀數（設計 4，舊 queue 8）。
3. P6 用雙緩衝還是三緩衝（三緩衝多 38.4 KB，換取吸收抖動）。
4. P8 的 tile 大小（16 KB 是上限估計值，要用 LCD 實際格式驗）。
5. P9／P10 的 `handover` 要不要一個「歸還期限」（防忘記還）。
