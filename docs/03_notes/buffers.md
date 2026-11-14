# 緩衝區（`nc_buf`）—— 全項目唯一一個緩衝工具

> 2026-11-10。**性質：原則已定案，程式未實作（P0）。**
> 這頁取代下列四份的**設計部分**，它們保留作歷史與量測紀錄：
> [buffer-manager.md](buffer-manager.md)（三階段／四種機制）、
> [buffer-manager-status.md](buffer-manager-status.md)（舊三原語現況）、
> [buffer-pool.md](buffer-pool.md)（small／bulk／forward）、
> [comm-buffers.md](comm-buffers.md)（轉發走 hub）。
> 仍然有效、不要合併：[memory.md](memory.md)（heap class）、
> [psram-surface.md](psram-surface.md)（PSRAM 的 pass 策略）、
> [buffer-tightening.md](buffer-tightening.md)（實測數字）、
> [tx-path-design.md](tx-path-design.md)（各 transport 的 TX 成本）。
> 字眼：[glossary.md](glossary.md)。
>
> **2026-11-10 修訂（未寫入本頁正文，實作前再改）**：
> ① **quantum 撤回** —— 只准「整條 buffer 換 owner」，唔需要再細分；
> ② 「釋放」＝**換手**（記憶體一動不動），唔是還格；
> ③ 每格新增 **`off`／`len`**（實際使用範圍），`cap`／`len`／`gen` 一律 **uint32**；
> ④ 怎麼用、有哪些技巧 → [buffer-patterns.md](buffer-patterns.md)。

---

## 1. 一句話

**一個工具：`nc_buf` = N 個等長格 ＋ 每格一個狀態。**
「在哪個記憶體」（S／P／DMA）同「是不是串流」是**參數**，不是不同的型別。

現在有四個名字指同一類東西（plan／pool／hub／環），所以 STATUS 的帳散在四個地方、
每加一條線都要重新決定「這次用邊種」。統一之後只剩一個。

---

## 2. 三條鐵律（定案）

| # | 規則 | 為什麼 |
|---|---|---|
| **R1** | **開機一次要齊**：所有 `nc_buf` 在開機建立，格數與格大小定死 | 執行期不確定性 = 0；`n` 由 config 決定，量過才改 |
| **R2** | **永不釋放**：冇 `nc_buf_destroy()`。唯一例外：執行期建立的（`NC_BUF_F_RUNTIME`）可以用 `nc_buf_release()` 還返；開機建立的一律拒絕 | **冇 free 就冇碎片** |
| **R3** | **執行期只准「切」唔准「要」**：runtime 要新 buffer，只能在已預留的 arena 內 `nc_buf_carve()`（★ 未實作）；唔准向 heap 要 | 同 R1／R2 一齊保證零碎片 |

**代價（要接受）**：執行期唔可以「加大」buffer。要加大就改宣告、重開機。
這同現有「冇 `nc_task_set_core()`、要換核就重開機」是同一種取捨。

### 2.1 為什麼沒有碎片 —— 講清楚，不要再靠 plan

舊文件把防碎片寄託在 `nc_plan` 的「每 class 一次 carve」。**唔準確**：plan 從來唔 free，
所以真正的原因是「冇 free」，唔是「一次 carve」。只要守住 R1–R3：

- 開機階段：一堆 `malloc` 但冇 `free` → 等於連續配置，唔會產生洞；
- 執行期：唔准 `malloc`／`free` → 帳面永遠不變；
- 唯一風險：**開機時大塊連續記憶體拿唔到**（heap 已被其他子系統切碎）→ 所以 arena 要在開機早期一次拿。

### 2.2 plan 的去向

`nc_plan` 不再是使用者要認識的概念。它降級成 `nc_buf` 內部的 **arena**：

```
開機早期：arena_S = 一次要一大塊 internal；arena_P = 一次要一大塊 PSRAM
建立 buffer：nc_buf_create() 從對應 arena 切 N × 格大小
```

保留 plan 的三個好處（逐項可失敗、有名字、總量睇得到），但對外只剩 `nc_buf`。
`nc_plan_*` 公開 API 在 P5 移除。

### 2.3 換 personality 不會「先租後還」——USB 實例（2026-11-13）

S3 的 OTG 與 USJ 共用同一條 FSLS PHY；但「換人格」是**重開機**的決定：
config 在 B02 讀一次、B03 定 USB 人格、B07 之後才宣告 plan。所以一次開機
**只會宣告其中一個** USB parser 租約；另一個從未存在 —— 不是「先租後還」，
而是「一開始就只租一個」。

而且兩種 USB 的 parser 租約**尺寸一樣**（`NC_USB_RX_REGION` = 4,128）、
宣告位置一樣，連租約名都一樣（`usb.rx`）—— 所以換人格之後 plan 的 layout 不變。
（舊名 `nc4.rx`（OTG）／`nc4.jtag`（USJ）已於 2026-11-13 統一成 `usb.rx`：
兩者本來就是同一個東西 —— USB 線的收幀／parser 環。）

同一原則也用在開機尾段的臨時字串（carve 之後**不准再碰 heap**）：

| 位置 | 以前 | 現在 |
|---|---|---|
| decoder 回覆幀 | `nc4_decoder_create()` 先 malloc 2,615 B，B09 再被 plan 緩衝換走並 `free` → **carve 之後才 free**，在 heap 留洞 | create 不配；回覆幀只由 plan 的 `nc4.reply0/1` 提供（`nc4_decoder_set_reply`） |
| plan JSON（700 B）、任務帳本（900 B） | 各自 `malloc`＋`free` | 共用一塊 static `s_boot_txt[900]` |

**仍然會在開機階段 free 的只有一處**：log 環由 2 KB 換成 8 KB（只在
`log_debug_mode=1` 時），發生在 plan carve **之前**，所以不影響租約 layout。
要完全消除，就要在 B01 之前讀到 log level（目前要等 B02 的 config）。

---

## 3. 概念（只需識三個屬性 ＋ 五個動詞）

**屬性**：`name`（帳面名，全系統唯一）／`where`（S／P／DMA）／`slot_bytes × n`（格大小 × 格數）

| 動詞 | 意思 | 對應舊概念 |
|---|---|---|
| `put_begin` / `put_end` | 拿一格自己寫滿，再發佈 | hub 的 `write_begin`／`commit` |
| `put(src, len)` | 一次過寫入（自動處理跨空間 copy） | 新增 |
| `get` / `get_end` | 拿下一格（或任意一格），用完還 | hub 的 `read_begin`／`read_end`；pool 的 `alloc`／`release` |
| `carve` | 由一段區域切一條新 buffer（呼叫端保管 cursor）；`.fxe` 換場用 | plan 的宣告 |
| `write`／`read_ptr`／`skip` | **串流模式**：部分寫、看連續一段、部分消費（重同步＝`skip(1)`） | 解析器環 |
| `put_abort` | 放棄正在寫的那一格（producer 中途失敗） | 新增 |
| `release` ★ | 只准還「執行期建立」的 buffer（`NC_BUF_F_RUNTIME`）；開機建立的拒絕 | parser 隨通道開關 |

**形狀**（只有兩種）：
- **FIFO**（預設）＝ 嚴格先入先出、單寫單讀（今日的 hub、轉寄、LED 雙緩衝）
- **STREAM**（`NC_BUF_F_STREAM`）＝ 一條連續位元組流，可以部分寫、部分讀、重同步。
  預設「線性＋memmove 壓實」；加 `NC_BUF_F_RING` 就係真環形（parser 環，2026-11-14）。

> **「隨機／任意空格」模式已移除（2026-11-10）**：冇真實用戶，而且它要多一個 `WRITING`
> 狀態（writer 死掉就卡死整條環）。`nc_pool` 將來要用「緩衝池」語義時，再按實際需求決定。
  `slot_bytes` 當環容量，`n` 必須 1；**唔支援格 API**（`put_begin`／`get` 會回 NULL）。

> FIFO 模式**唔會**把正在寫的格標成 WRITING：producer 在 `put_end` 之前死掉，只會令
> 寫指標唔前進，下一輪由同一格重寫 —— 環永遠唔會卡死。這條是照
> `nc4_bus.c` 的實測經驗抄的（「少一個狀態就少一個可以卡死的地方」）。

---

## 4. 四種空間組合（S2S／S2P／P2S／P2P）

**一個 buffer 只有一個位置，不會雙份。** 四種組合不是四種 buffer，而是
「**格在哪** × **資料由哪來**」的四種引數，全部由同一個 `put()` 處理：

| 格在哪 | 資料由哪來 | 叫法 | 4 KB | 38.4 KB | DMA 可用 | 這個項目哪裡會用 |
|---|---|---|---:|---:|---|---|
| S | S | S2S | ~0.02 ms | 0.15–0.3 ms | ✅ | RX → parser → handler；I80 波形 |
| P | S | S2P | ~0.13 ms | ~1.2 ms | — | 轉發（internal RX → PSRAM 交棒） |
| S | P | P2S | ~0.13 ms | ~1.2 ms | ✅（copy 後） | 回覆 JSON（P）→ TX 環（S）；render tile → DMA |
| P | P | P2P | ~0.2 ms | ~2.4 ms | ❌ | tap／第二個消費者 |

- 數字來自 §7 的量測（internal copy 133–252 MB/s、PSRAM 20–33 MB/s）。
- 判斷來源空間**唔需要 flag**：用 `esp_ptr_external_ram()`／`esp_ptr_dma_capable()`
  由指標本身決定（`esp_memory_utils.h`），所以呼叫端寫 `nc_buf_put()` 就夠。
- **DMA 那條線是硬規則**：`DMA` class 的 buffer，`put()` 收到 PSRAM 指標一定 copy 入 S。
  今日「S3 I80 DMA 只能讀內部 SRAM」靠人記住，以後靠型別擋。
- 每個 buffer 記四個方向的 bytes 與累計時間 → `mem.buf` 直接睇到「這條線花咗幾多 copy」。

### 4.1 零複製：三條路 ＋ 一個監測

「可唔可以直接交換地址」要先分清楚係邊一種交換：

| 情況 | 做法 | 要唔要抄 |
|---|---|---|
| **同一個 buffer**：生產者寫、消費者讀同一格 | 生產者 `put_begin` 寫入、消費者 `get` 借用 | **零複製**（指標本來就係同一塊） |
| 跨 buffer、**壽命唔同**（parser 環 → 轉寄） | 抄一次 | 一定耍抄 |
| **整條 buffer 換用戶**（`.fxe` 換場、OTA） | `nc_buf_handover()` | **零複製**（記憶體一動不動） |
| 跨 buffer 交棒、但想唔抄 | descriptor 交接 ＋ refcount | 技術上可行，**本專案唔採用** |

**為什麼唔做 descriptor 交接**（把「指向別人那格」當成自己的一格）：

1. 來源格會被 **pin 住**到讀者還為止 —— 轉寄正是「來源核唔可以等」的場景，一 pin 就塞住 parser 環。
2. 讀者要多一層間接（先讀 descriptor 再跳去另一條 buffer），熱路徑變慢。
3. 跨核要處理 cache 可見性與 lifetime，等於把 `nc_hub` 的 generation 機制搬進每一格。

**工具自動判斷到哪一步**：`src_buf_of()` 會查來源指標落在**哪條 buffer、哪個空間**，
所以工具知道「你正在從另一條 buffer 抄」。它**唔會自動幫你改寫成零複製**
（那會改變邊個擁有記憶體，唔應該靜靜發生），但會記一個 **`xcopy`**：

- `xcopy > 0` ⇒ 你抄了「自己另一條 buffer」→ 通常可以改成「直接寫入目的地」或「借用」。
- `xcopy = 0` 而其餘 `s2p`／`p2s` > 0 ⇒ 來源唔係我們的 buffer（RX 區、堆疊、driver），
  呢種 copy 係真嘅，逃唔掉。

---

## 5. 佈局：怎樣搭配才釋放得到性能

### 5.1 七條原則

1. **熱路徑 → S**（每幀都掃、每個命令都過的：RX parser、命令 staging、TX 環）。
2. **冷路徑 → P**（一次過、大、慢：轉發 staging、LED frame、`fx.arena`、回覆幀）。
3. **DMA → 一定 S**，而且 `DMA` class（對齊 32／64）。
4. **PSRAM 頻寬是全核共用的**：兩核同時 copy 會由 20–21 腰斬到 **10 MB/s**。
   → PSRAM copy 集中在一個核、一個時段；另一核避免同時掃 PSRAM。
5. **大幀唔好掃多次**：>32 KB 的 frame 每次進出 PSRAM 都是 1.2 ms（38.4 KB 實測）。
   做法是 copy 入 **S tile（≤16 KB）** 做完 multi-pass，再一次寫回 P ——
   分塊只值 1.3×（16 vs 12 MB/s），它的價值是 **bound latency** 與 **局部性**，不是頻寬。
6. **跨核交棒走 P，但只有來源核寫、目的核讀**（單寫單讀，strict FIFO）。
7. **flash 寫入會停 cache → PSRAM 一齊停**。所以 PSRAM 上唔好放有實時要求的東西；
   OTA／檔案寫入期間，PSRAM 之上的路徑（含轉發）會一齊等，這是已知代價。

> **唔好信的三個數字**：16 KB PSRAM 的 152 MB/s 是「同一塊重複 copy」的 cache 假象；
> 理論 160 MB/s 從未達到過；`tile 16 KB` 唔等於「切細就快 6×」。

### 5.2 S3 的建議佈局（現況 ＋ 目標）

| buffer | where | 格 × N | 總量 | 理由 |
|---|---|---|---:|---|
| `usb.rx`（USB parser） | S | 4,128 × 1 | 4.0 KB | 熱路徑，逐位元組掃 |
| `uart.rx` | S | 1,024 × 1 | 1 KB | 同上 |
| `tcp.rx`（P4） | S | 8,205 × 2 | 16.4 KB | 同上（2 幀餘裕） |
| `uart.tx`（環） | S | 1,024 × 1 | 1 KB | driver 要 S；4 KB 轉寄會阻塞 44 ms @921600 |
| `log`（兩核） | S | 8,192／1,024 | 9 KB | 熱路徑寫入，consumer 慢 |
| `reply`（每核一格） | **P** | 2,615 × 2 | 5.1 KB | 現時是 2 份 heap，唔在帳內；改成一個 n=2 的 buffer |
| `fwd`（轉發） | **P** | 4,160 × 32（16 條 × 2） | 130 KB | 跨核交棒；來源核只寫 0.2 ms／4 KB |
| `fx.arena` | **P** | 524,288 × 1 | 512 KB | `.fxe` 從這裡 carve（R3） |
| `（→）usb.rx`／`uart.rx` 的形狀 | S | STREAM＋RING | 4,128／1,024 | 2026-11-14 起 parser 環係真 RING（外部 `create_in` 仍可線性；§12） |
| `small`（命令 staging） | S | 1,024 × 8 | 8 KB | **目標：預設關**（現況開住）；等 header-first assembler 真正用才開 |
| `bulk`（OTA／FILE） | S 或 P | 4,160 × 3 | 12.2 KB | **目標：預設關**；大而冷，可放 P |
| `pool.forward`（現況，**P3 移走**） | S | 4,160 × 2 | 8.3 KB | 跨核轉寄的舊路徑；P3 由上面 `fwd` 取代 |
| `led.frame`（未實作） | **P** | 38,400 × 2 | 75 KB | 雙緩衝，render 寫、I80 讀 |
| `led.wave`（未實作） | **S** | 28,800 × 2 | 56.3 KB | I80 DMA 只能讀 S |
| `pixel.tile`（未實作） | **S** | 16,384 × 1 | 16 KB | multi-pass 工作區（原則 5） |

**合計**：internal ≈ **44 KB**（4.1 ＋ 1 ＋ 1 ＋ 9 ＋ 8 ＋ 12.2 ＋ 8.3，含現況的 `forward` 池 —— P3 移走後約 36 KB），
未計棧與靜態分區；目標再新增 `led.wave`／`pixel.tile` 72 KB。
PSRAM ≈ **647 KB**（回覆 5.1 ＋ 轉發 130 ＋ fx 512），佔 8 MB 的 **7.9%**。
上板 `sys.h_int` 現值 237,403 B（tests 映像）—— internal 仍然有大量餘裕，
所以**新東西優先放 PSRAM 是對的，但要記住原則 4**。

### 5.3 搭配的三個具體決定

1. **回覆幀合併成一條 `reply`（P，n=2）**：✅ 已完成（P2）—— 現時是 plan 的
   `nc4.reply0/1`（各 2,615 B），decoder 用 `nc4_decoder_set_reply()` 接管，
   自己**不再** `heap_caps_malloc`（見 nc4.c 的 create 說明）。
2. **轉發用 16 條 FIFO buffer（P）不是一條**：per（目的 link × 來源核）。
   一條的話，慢 UART 會 head-of-line 塞住同核的 TCP。
3. **`small`／`bulk` 預設 `enable:0`**：現時冇 header-first assembler 接上，
   開著只是白佔 internal。要用才在 `system.json` 開。

---

## 6. 介面（P1 已落，★ = 未實作）

```c
// 開機：一次要齊（內部向 arena 拿）
nc_buf_t *nc_buf_create(const char *name, nc_mem_t where,
                        size_t slot_bytes, int n, uint32_t flags);
// ★ 執行期：只可以在 arena 內切（未實作，P5／fx 換場時才加）
nc_buf_t *nc_buf_carve(nc_buf_t *arena, const char *name,
                       size_t slot_bytes, int n);

// 寫
void     *nc_buf_put_begin(nc_buf_t *b);              // 冇空格 → NULL，永不等待
esp_err_t nc_buf_put(nc_buf_t *b, const void *src, size_t len);   // 自動 S/P copy
esp_err_t nc_buf_put_end(nc_buf_t *b, size_t len);    // 發佈

// 讀
void     *nc_buf_get(nc_buf_t *b, size_t *len);       // 冇 READY → NULL
esp_err_t nc_buf_get_end(nc_buf_t *b);                // 還格

// 帳
size_t    nc_buf_json(char *out, size_t cap);         // STATUS keys=mem → mem.buf
```

- `flags`：暫時只有 **`NC_BUF_F_FIFO`**（嚴格順序、單寫單讀）。
  **DMA 不是 flag，是 `where`**：`where = NC_BUF_DMA`（對齊 64、只准 internal）。
  其餘（tap／多讀者）P3 才加。
- 上限：`NC_BUF_MAX 32`（registry 32 × 8 B）。**滿了一定要回 NULL 並 `ESP_LOGE`，唔可以靜默** ——
  這正是 `nc_hub` 今日的缺陷（registry 滿只係唔登記）。
- 每格 descriptor 8 B（狀態 ＋ generation）；1,024 格 = 8 KB internal。
  → **格數幾乎唔值錢，格大小才是錢。**

---

## 7. 帳面：`STATUS keys=mem` 只剩一個 `mem.buf`

```json
{"mem":{"buf":{"arena_S":{"total":65536,"used":44032},"arena_P":{"total":8388608,"used":662638},
  "list":[{"name":"fwd","where":"P","slot":4160,"n":32,"fill":0,"bytes":133120,
           "s2p":0,"p2s":0,"s2s":0,"p2p":0,"fails":0,"viol":0}]}}}
```

- 取代 `mem.pool`／`mem.hub`／`net.links`／plan 四份分散的帳。
- `s2p`／`p2s`／`s2s`／`p2p` = 各方向累計 bytes（§4 的帳）；`viol` = 單寫單讀被破壞次數；
- `xcopy` = 由**另一條 buffer** 抄過來的 bytes（§4.1）—— 呢個數字應該盡量係 0。

---

## 8. 遷移（每一階段都可獨立驗收）

| 階段 | 內容 | 驗收 |
|---|---|---|
| **P0** | 本頁 ＋ [glossary.md](glossary.md)；不動程式 | 文件可讀、術語一致 |
| **P1** | `nc_buf` 核心：FIFO ＋ STREAM ＋ S/P copy ＋ `mem.buf` 帳；`NC_BUF_MAX`、滿了回 NULL | hosttest；上板 STATUS 與實際 bytes 一致 |
| **P2** | 回覆幀、TX 環、`small`／`bulk` 搬過去（行為不變） | 每次只搬一項；吞吐／RTT 與搬之前一樣 |
| **P3** | 轉發搬到 16 條 FIFO buffer（取代 `nc_pool.forward` ＋ 跨核 queue）；舊路徑留 A/B | CDC→ESP-NOW／P4→TCP 轉發；`drop_*` 分開計 |
| **P4** | per-link parser 環搬過去（同 header-first assembler 一齊做） | 五通道收幀不變；`mem.buf` 見到每條 link |
| **P5** | 刪 `nc_plan_*` 公開 API；`nc_task_res_t` 改成「宣告 buffer」 | 全 repo 冇 `nc_plan_` 呼叫 |

**明確不做**：執行期加減 buffer、`free`、多 size class 的「自動選最細夠用」分配器
（要分 class 就叫兩個 buffer，明寫）。

### 8.1 現況（2026-11-10 上板）

| 項目 | 狀態 |
|---|---|
| `nc_buf` 機制（`firmware/components/sys/mem/nc_buf.{h,c}`） | ✅ **已實作**：FIFO（唯一格模式）＋ STREAM（線性）＋ RING、`put_begin(reserve)`、`put/put_end(len)`、`get/get_end`、`handover`、`nc_buf_json` |
| `off`／`len`／`gen`／`cap` 32-bit | ✅（`nc_pool` 的 uint16 是對照組：LCD 一幀會爆、`gen` 13 秒 wrap） |
| `STATUS keys=mem` → `mem.buf` | ✅ 上板 858 B／上限 2,600；零值欄位省略，放唔落會標 `truncated` |
| 上板驗收 | ✅ `0xF032 BUF_TEST` 9 項全 pass；`tools/buf_test.py` 一鍵跑 |
| hosttest | ✅ `tools/hosttest/buf_test`（9 組、全綠；含 registry 滿、JSON 截斷） |
| `nc_buf_carve()`（`.fxe` 換場） | ✅ **已實作（2026-11-10）**：無狀態 bump，64 B 對齊、唔夠位回 NULL；hosttest 有覆蓋 |
| **宣告路徑（P5）** | ✅ **已完成（2026-11-10）**：`nc4.reply0/1`、`usb.rx`、`uart.rx`、`fx.arena` 全部由管理器建立，`on_start` 收 `res[i].buf`；**全 repo 已冇任務 call `nc_plan_get()`** |
| arena（把 `nc_plan` 吸收） | ⏸ `nc_plan` 只剩管理器與開機 commit 在用（對外等於內部實作） |
| **回覆幀**搬過來（P2） | ✅ **已完成（2026-11-10）**：`nc4.reply0/1`（plan 宣告、P、各 2,615 B、n=1），decoder 用 `nc4_decoder_set_reply()` 接管 |
| **bus 插槽環／chunk**（P3） | ✅ **已完成（2026-11-10）**：`<link>.slots`（FIFO），decode 直接由 slot 解析（唔再經 `chunk`）；手寫的 `slot_state[16]`／`slot_len[16]`／w·r·push_idx 全部刪除 |
| **`nc_pool` 與跨核轉寄佇列**（P4） | ✅ **已完成（2026-11-10）**：`nc_pool` 整個刪除（small／bulk 零用戶但仍佔 28.3 KB internal）；轉寄改成每核一條 `nc_buf` FIFO（`fwd.c0`／`fwd.c1`，PSRAM、按需建立、格首 16 B 放目的 link 名） |
| `nc_hub`／TX 環（P5） | ⏸ `nc_hub` 只剩測試用戶；TX 環由 driver 擁有（唔搬） |
| `handover` 在生產路徑使用 | ⏸ 未做（現時只有測試用 `t.sram`／`t.psram`／`t.stream`） |
| **STREAM 模式**（連續位元組、部分寫／讀） | ✅ **已實作**（2026-11-10）；**RING**（真環形、唔壓實）2026-11-14 加 |
| **`put_abort` ＋ FIFO 唔標 WRITING**（唔會卡死環） | ✅ **已實作**（同上） |
| 總線（`nc4_bus`）搬過來 | ⏸ 未做（§11） |

**與本頁設計的三處差異（已實作、要記住）**：
1. 沒有 quantum／bitmap（依 §頂部修訂：只准整條換手）。
2. JSON 省略零值欄位，並去掉可推導的 `bytes`／虛榮的 `puts`／`gets`
   —— 否則 13 條 buffer 會把 `keys=mem` 撐爆。
3. 測試 buffer 建一次之後**留在 registry**（`t.*`，3,072 B）；因為鐵律係「冇 destroy」。

---

## 9. 任務 ↔ 緩衝區契約（規範）

> 這一節是**規範**：寫任務的人、改管理器的人、之後改 `nc_buf` 的人，都以此為準。
> 標記：✅ 今日已成立　⏳ 要等 P5（`nc_task_res_t` 改成宣告 buffer）

### C1 宣告（誰要）✅

任務在 `declare()`（或 `mem[]`）回報它要的**每一條**緩衝區：`name`、`where`（S／P／DMA）、
`slot_bytes × n`、`flags`。

> ⏳ P5 之前 `nc_task_res_t` 只有 `name`／`bytes`／`cls`／`align`，所以
> `slot_bytes × n` 要自己算成一個 bytes 數；`where` 用 `cls` 表達（對照表見
> [writing-a-task.md §3.1](writing-a-task.md)）。

- 名字**全系統唯一**，前綴用**擁有者**（不是協議名）：
  - 線的收幀環 → 線名：`usb.rx`／`uart.rx`／`tcp.rx`
  - 任務的緩衝 → 任務名：`fx.arena`／`probe.hub`
  - 協議引擎每核的狀態 → 協議引擎名＋核號：`nc4.reply0`／`nc4.reply1`
- **NC4 是協議**（分幀、指令、回覆），不是通道、不是 owner —— 所以一條線的
  收幀緩衝**不可以**叫 `nc4.*`（那正是舊名 `nc4.rx`／`nc4.jtag` 的錯）。
  這樣 `mem.buf` 一眼看得出邊個的。
- **`declare()` 裡唔准** `malloc`、唔准 `nc_buf_create()`（那時管理器未排序、未檢查重名）。

### C2 建立（誰建）✅

管理器在 `start_all` 之前、依 `level` 由細到大逐項建立。

- **建立是逐項的，啟動是全有或全無**：一項要不到，其他項照樣建立，
  但**整個任務不會啟動**，`on_start` 根本不會被呼叫
  （[task-lifecycle.md §2 ③](task-lifecycle.md)）。
- 所以任務見不到「少一項」的世界；仍然建議在 `on_start` 檢查 `n`（防禦 runtime attach）。
- 代價：已建立的項**不會退回**（冇 free）—— 那塊記憶體就永久屬於那條 buffer，
  `mem.buf` 與開機 log 睇得到，但唔會自動消失。

### C3 交付（幾時拿到）✅

`on_start(res, n)` 是**唯一**拿到緩衝區的地方；次序 = `mem[]` 先、`declare()` 後。
只在**全部資源都拿到**時才會被呼叫（見 C2）。

- ⏳ P5 之後 `res[i]` 的型別由 `{ptr,len}` 變成 `nc_buf_t *`。
- 今日仍是 `{ptr,len}`（區域），任務自己用 `nc_buf_at()` 接管；
  兩種寫法都**只可以在 `on_start` 拿**。
- 唔准用 `nc_buf_find()` 去拿**別人**的 buffer（唯一的例外：重啟後拿回自己上一世的）。

### C4 生命（幾時消失）✅

`on_stop` 之後緩衝區**仍然存在**（鐵律：冇 destroy）。

- `nc_task_stop()` → `nc_task_start()` 會**重用同一批指標**；
  所以 `on_start` 只跑一次，指標要放 `static`。
- 只有重開機會重新建立。這一條是「零碎片」的代價，寫任務的人要知。

### C5 擁有權（誰可以換手）✅

每條緩衝區有一個 **owner**（預設 = 名字）。

- 只有 owner 可以 `nc_buf_handover()`。
- 換手前**所有格必須 FREE**；換手後 `generation++`，舊把手立即失效。
- 換手**唔搬記憶體**，所以 owner 換幾多次都不會產生碎片。

### C6 讀寫權（幾個 writer／reader）✅

| 模式 | 契約 |
|---|---|
| FIFO（唯一格模式） | **一個 writer、一個 reader**。多個 writer 要自己用 mutex 序列化 —— 工具唔會幫你，但會用 `viol` 計「同一格被兩個讀者拿」 |

- 跨核只傳**資料**，唔傳喚醒（[execution-rules.md §2](execution-rules.md)）。
  讀者有冇被喚醒、幾時來讀，係用家自己的事；工具只保證 FIFO 次序。

### C7 熱路徑 ✅

- 熱路徑只准 `put`／`put_begin`／`put_end`／`get`／`get_end`。
- `create`／`handover` 只可以**開機**或**換場**（`.fxe` 換模組、OTA 開始）。

### C8 對外可見 ✅

- 緩衝區的帳**以 `STATUS keys=mem → mem.buf` 為準**；任務唔可以另外開一份
  「我的緩衝表」——有第二份帳就一定有對不上的時候。
- 要讓別的任務讀內容 → 用 `provides[]`（`NC_PROVIDE_BUF`），**唔好把 `nc_buf_t *` 周圍傳**。
  provide 的生命週期綁在提供者身上（stop／detach 就消失）。

### C9 執行期任務（`.fxe`）✅

- **唔可以宣告**緩衝區（宣告窗口在 commit 關閉）。
- 只可以由父層 arena `nc_buf_carve()`（★ 未實作，P5 前用 `nc_buf_at()` 接管既有區域）切。
- 卸載**唔會**還記憶體；要給下一個模組就 `handover`。

### C10 上限與失敗 ✅

- 全系統 `NC_BUF_MAX` **32 條**；滿了 → 建立失敗 ＋ `ESP_LOGE`（**唔靜默**）。
- 一個任務最多 `NC_TASK_MEM_MAX`（8）項。
- 判斷一項有冇成功：`mem.buf` 少一條就是失敗（唔會用 0 值假裝存在）。

### C11 驗證 ✅

每個任務的緩衝區用法要對得上 [buffer-patterns.md](buffer-patterns.md) 的其中一個模式；
新增一種用法 → 先在該頁加一個模式，再寫程式。上板驗證用 `0xF032`／`mem.buf`。

### 9.1 現況對照（2026-11-10）

| 契約 | 今日 | 缺口 |
|---|---|---|
| C1 宣告 | 已帶 `slot_bytes`／`n`／`flags`（2026-11-10 P5 完成）；宣告了就由管理器建 `nc_buf` | — |
| C2 建立 | plan 逐項 carve、逐項可失敗 | P5 改成 `nc_buf_create`（`where` 由 `cls` 對應） |
| C3 交付 | `on_start(res_{ptr,len})` | ⏳ P5 才交付 `nc_buf_t *` |
| C4 生命 | 已經係「唔還」 | — |
| C5 換手 | `nc_buf_handover` 已實作，未有生產用戶 | 等 P3（`.fxe` 換場） |
| C6 讀寫 | FIFO 已實作、`viol` 已計 | 冇自動序列化（刻意） |
| C7 熱路徑 | 契約已寫 | 未有 CI 檢查 |
| C8 帳面 | `mem.buf` 已上板 | plan／pool／hub 三份舊帳仍然並存（P5 收掉） |
| C9 `.fxe` | 今日用 `nc_fx` 自己的 arena 切 | 未改成 `nc_buf_carve` |
| C10 上限 | 已實作（滿了回 NULL ＋ log） | — |

### 9.2 為什麼是這些條，不是更多

- **唔定「幾時可以 put」**：那是模式（P1–P10）的事，唔是機制。
- **唔定「buffer 內容格式」**：工具只認 bytes；NC4 幀、LED 幀、JSON 都是你的語意。
- **唔提供 destroy**：一旦有 destroy，就要面對碎片與「誰還在用」兩個問題；
  換手已經覆蓋「下一個任務重用同一個區域」這個真實需求。



---

## 10. 未定

1. **可等待嗎**：現時一律非阻塞（`NULL` = 冇格）。要不要提供「等到有格／有資料」（給低頻 consumer 省電）？
2. **tap／多讀者**：refcount 與 generation 已有前身，但未接。等第二個消費者真的出現再做。
3. **`NC_BUF_MAX` 定 32 夠唔夠**：測試任務（mesh 12 條）會逼近；要麼測試自己收斂，要麼升到 48（+128 B）。
4. **per-link `max_frame`**：現時全域；量到某條線特別大再拆。
5. **arena 的切法**：一個大 arena 切，還是 S／P 各一個？（現建議各一個）
6. **C3 的型別轉換**：P5 把 `on_start(res)` 由 `{ptr,len}` 改成 `nc_buf_t *` 時，
   舊任務要一次過改；要不要提供 `res.view` 相容期？

---

## 11. 總線（`nc4_bus`）的對照 —— 它暴露了工具兩個缺口

2026-11-10 盤點：總線今日擁有**三種形狀**的緩衝，其中一種是同一個工具的**第二份手寫實作**。

| 總線的東西 | 今日放在哪 | 形狀 | 對 `nc_buf` |
|---|---|---|---|
| **PUSH 插槽**（TCP／WS／UDP 之類） | `nc4_bus.c`：`slots` ＋ `slot_state[16]`（0 free／1 ready／2 writing／3 reading）＋ `slot_len[16]` ＋ `slot_w`／`slot_r` | N 個等長格、FIFO | **同 FIFO 模式一模一樣** → 應該搬（C8：帳面只可以有一份） |
| **PULL parser 環**（CDC／UART） | plan 的 `usb.rx`／`uart.rx` ＋ `nc4_parser_feed`（RING；跨環 frame 線性化到 per-core scratch） | 連續位元組、部分讀、壞幀前移 1 byte | **STREAM＋RING**（2026-11-14） |
| **TX 環** | 各 transport／driver 自己 | 位元組環 | **唔搬**：driver 擁有，它自己管 DMA／FIFO |
| 回覆幀 | plan 的 `nc4.reply0/1`（2,615 B ×1/核） | 單格 | ✅ 已搬（P2） |

### 11.1 總線指出工具**兩個要改**的地方（都已改）

1. **缺 STREAM 模式** —— parser 環係「部分寫、部分讀、可以前移 1 byte 重同步」，
   完全唔係格。今日補了：`write`／`read_ptr`／`skip`／`pending`／`space`，
   2026-11-10 先用「線性＋`memmove` 壓實」；2026-11-14 再改成 **RING**（`st_in/st_out`、
   `nc_buf_read_segments`），跨環 frame 只抄該幀到 per-core scratch。
2. **`put_begin` 會標 WRITING → producer 死掉就卡死整條環**。
   總線的註解明講「少一個狀態就少一個可以卡死的地方」，所以它刻意唔標。
   工具已跟：**唔標 WRITING**，並補 `put_abort()`（錯誤路徑要明確放棄）。

### 11.2 搬遷順序（建議）

| 步 | 內容 | 風險 | 為什麼排這裡 |
|---|---|---|---|
| **B1**（**2026-11-10 第二次更正**） | **decode 直接由 slot 解析**（`slot_peek` → `nc4_parser_feed(slot)` → `slot_release`），省掉 `slot→chunk` 那一次 copy；slots 保留 | 低 | ⚠️ **唔可以**叫 producer 直接寫（線性）STREAM 環：線性 STREAM 寫入時會 `memmove` 壓實，而 decode 在 `parser_dispatch()` 期間仍然揸住 ring 的指標 → producer 一壓實，資料就在指標腳下移動（靜默損壞）。現時的 slots 正好把「寫」與「讀」分開，係**正確**的設計。**RING（2026-11-14）冇這個限制**：producer 只寫 `in` 側，唔會移動 consumer 正在讀的資料。（2026-11-10 追加：`slot_full` 的六成丟幀其實是**producer 優先級高過 decode**，把 producer 降到 decode 之下就變 0、喚醒延遲 478→62 µs。見 [status §7.39](../00_pm/status.md)）|
| **B2** | parser 環改用 `nc_buf`（STREAM → **RING**） | 中 | ✅ **2026-11-10 用 STREAM；2026-11-14 改用 RING**（見 §12.5）：跨環 frame 只抄該幀到 per-core scratch，平常零複製；`0xF02F` 自我測試在 S3／P4 都回 true |
| **B3** | 回覆幀、TX 環 | 低 | 純帳面統一 |

**唔會搬**：framing 狀態（每條線自己的半幀位置）、TX 環（driver 擁有）、
`budget`／`chunk` 這些排程參數（它們不是緩衝區）。

**驗收**：每個通道收幀數不變、`net.links` 與 `mem.buf` 對得上、`0xF032` 仍然全 pass。

---

## 12. 量測與落地：slot FIFO vs ring STREAM（2026-11-14）

**怎麼重跑**：燒 `--tests` 映像，然後

```
tools/nc4.py --port <port> raw 0xF02B          # 預設 30,000 × 1,024 B
tools/nc4.py --port <port> raw 0xF02B <u32>    # 自訂 iterations（1,000–500,000）
```

量測在 decode0 的 handler 內跑：單 task、producer 與 consumer 同一個 loop、
資料在**內部 SRAM**、1,024 B/幀；每個 test 都做「寫入 → 消費」完整循環。
`ns`＝每幀；`mb`＝B/µs（十進位 MB/s）。

### 12.1 改動後（同一支 benchmark，兩塊板）

| test | 意思 | S3-Matrix 240 MHz | P4-C6 360 MHz |
|---|---|---|---|
| `slot_pct` | 最小格環，`% n` | 337.6 MB/s | 496.8 MB/s |
| `slot_mask` | 同上，`& (n-1)` | 335.0 | 494.8 |
| `slot_mask_f` | 同上＋release/acquire fence | 335.5 | 487.5 |
| `fifo_real` | **`nc_buf` FIFO**（零複製交格） | **277.1** | **419.4** |
| `stream_real` | **`nc_buf` STREAM**（線性＋壓實，無 backlog） | **279.2** | **325.6** |
| `stream_real_wrap` | 同上，backlog ≈ 環一半 | **18.3** | 203.5 |
| `kfifo_frame` | kfifo 式 byte ring（copy in＋copy out） | 178.4 | 255.4 |
| `stream_kf` | byte ring（copy in＋原地掃描，無 backlog） | 354.2 | 506.4 |
| `stream_kf_wrap` | byte ring，backlog ≈ 環一半 | 346.0 | 530.2 |
| `lin_nowrap` | 最小線性實作，無 backlog | 355.0 | 513.5 |
| `lin_wrap` | 最小線性實作，backlog ≈ 環一半 | 18.6 | 276.0 |
| **`ring_real`** | **`nc_buf` RING（新，opt-in）**，無 backlog | **264.6** | **312.7** |
| **`ring_real_wrap`** | **`nc_buf` RING，backlog ≈ 環一半** | **257.0** | **306.1** |

### 12.2 這次實際改了什麼

| 改動 | 位置 | 結果 |
|---|---|---|
| FIFO publish／consume fence | `nc_buf_put_begin/put_end/get/get_end` | 正確性；FIFO 成本 −1.5%（S3）／−2.8%（P4） |
| `account_copy` 只掃一次 registry ＋ 整體位址範圍快篩 | `nc_buf.c` `src_buf_of`／`account_copy` | **STREAM 無 backlog：S3 174.7 → 279.2（+60%）、P4 271.4 → 325.6（+20%）** |
| 新增 `NC_BUF_F_RING`（`st_in/st_out`、`nc_buf_read_segments`） | `nc_buf.[ch]` | buffer backlog 下 S3 18.3 → 257.0（**14×**）、P4 203.5 → 306.1（**1.5×**）；parser 穩態則 ≈ 線性（見 §12.4） |
| RING 不壓實／不搬資料 | 同上 | 解決 §11.2「consumer 揸住指標時 producer 唔可以寫」的限制 |
| `% n` → mask | — | **唔做**：量測顯示冇收益 |
| parser 內部環改 RING＋跨環處理 | `nc4.c` `nc4_parser_create`／`nc4_parser_feed` | 跨環 frame 只抄該幀到 per-core scratch；平常零複製；`0xF02F` 驗證 |

RING 係 **opt-in**：`nc_buf_create(..., NC_BUF_F_STREAM | NC_BUF_F_RING, ...)`。
**2026-11-14：`nc4_parser_create()` 建立的 parser 環已改用 RING**；
外部傳入的 ring（`nc4_parser_create_buf/in`）可以仍然是線性 STREAM，
`nc4_parser_feed()` 兩種都支援（線性時 `n1 == 0`，走原本的一段路徑）。

### 12.3 讀出來的四件事

1. **`% n` → `& (n-1)` 冇用**：S3 337.6 → 335.0、P4 496.8 → 494.8（在噪音內）。
   除數不是瓶頸，1 KB 的搬運才是 → **不要做這個「優化」**。
2. **fence 便宜**：S3 FIFO 281.3 → 277.1（−1.5%）；P4 431.4 → 419.4（−2.8%）。
   為了跨核可見性，這個代價值得付。
3. **零複製格 FIFO 仍然贏**：`fifo_real` 277 vs `kfifo_frame`（copy out）178（S3）、
   419 vs 255（P4）。唔應該把 FIFO 改成 kfifo 的 copy 語義。
4. **RING 在 backlog 下大幅勝出**：S3 線性 18.3 vs ring 257.0（**14×**）；
   P4 203.5 vs 306.1（**1.5×**）。無 backlog 時 ring 反而略慢（264.6 vs 279.2；
   312.7 vs 325.6，−4～−5%），因為每次寫讀都要 `% slot_bytes` 與多一次函式呼叫。
   P4 的 memmove 快得多，所以線性版的崩塌冇 S3 咁嚴重。
5. **`lin_nowrap` ≈ `stream_kf`**（355.0 vs 354.2；513.5 vs 506.4）→ 線性本身唔慢，
   之前的 2× 差距主要是 API／記帳；`account_copy` 修完之後，`stream_real` 已經追回大半
   （279 vs 355，剩 ~21%）。

### 12.4 parser 遷移與驗證（2026-11-14）

`nc4_parser_create()` 的內部環已由線性 STREAM 改成 **RING**。
`nc4_parser_feed()` 不再假設「一定一段連續」：

1. 用 `nc_buf_read_segments()` 拿最多兩段，跨段找 SOF（包括 SOF 剛好跨邊界）。
2. 標頭 9 B 如果跨段，只抄 9 B 到 stack 暫存讀 `ver`／`dlen`。
3. 整個 frame 如果跨環，只把**這一幀**（≤ `NC4_MAX_FRAME` 8,205 B）抄到
   per-core scratch，再 dispatch；平常 frame 仍然零複製。
4. dispatch 完 `nc_buf_skip(total)`；下一輪重新拿 segments。

代價：`static uint8_t s_parse_scratch[cores][NC4_MAX_FRAME]` =
**8,205 B／core**（S3／P4 16.4 KB、C3 8.2 KB BSS；不佔 plan）。
好處：producer 可以在 consumer 揸住指標時繼續寫（RING 不會搬資料），
backlog 下亦不會出現線性版的 memmove 崩塌。

**驗證**：新指令 `0xF02F PARSER_SELFTEST` 做兩件事：

1. 建一個 256 B RING parser，餵兩個 frame（第二個由環尾繞回開頭），
   檢查 CRC／header 與 `n_max_ok`。S3／P4 都回 `{"parser_ring":true}`。
2. 同一批 2,000 × 1,024 B frame（每個切 3 段餵入）跑 RING vs 線性 parser：

| parser | S3-Matrix | P4-C6 |
|---|---:|---:|
| RING | 164.7 ms（12,146 fps） | 75.6 ms（26,446 fps） |
| 線性 STREAM | 163.9 ms（12,199 fps） | 75.2 ms（26,590 fps） |

兩邊都完成 2,000 幀；RING 只慢 **0.4–0.5%**。所以**穩態 parser 沒有吞吐優勢**：
RING 的價值是**最壞情況**（backlog／大量 pending 時免 memmove）與**所有權**
（consumer 揸住指標時 producer 可以繼續寫）；buffer-level 的 14× 是 backlog 壓力案例，
不是 parser 穩態。`0xF008` 的 `crc_err`／`bad_hdr` 為 0。

### 12.4 邊界

- 這是**微基準**：單 task、內部 SRAM、固定 1 KB 幀、冇跨核、冇真實 parser／driver。
  真實吞吐仍要以 `0xF008`／`buf_probe.py` 的通道量測為準。
- backlog 版是刻意把 pending 維持在環一半（`> cap/2` 就 skip 一幀），
  令線性版每次都踩到壓實；真實系統的 backlog 取決於 decode 是否追得上。
- P4 是用 ROM USB（USJ）連線跑的；偶有回覆遺失（與 iterations 無固定關係），
  但 5,000／20,000 兩輪數字一致。
