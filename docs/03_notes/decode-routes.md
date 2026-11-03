# 解碼路線與接收緩衝（現況）

> **狀態**：現況盤點（2026-11-02）。不是提案，是「現在程式真的怎麼跑」。
> **相關**：[rx-architecture.md](rx-architecture.md)（目標架構）、[memory.md](memory.md)（plan / hub）、[naming.md](naming.md)

---

## 1. 五條通道，五種路線

| 通道 | 誰在跑 | 核心 | 接收緩衝 | 分派方式 |
|---|---|---|---|---|
| **CDC** | `decode0`（bus, PULL） | 0（`bus.cfg.core`） | plan `nc4.rx` 8,205 + `chunk` 1,024（heap） | decoder 表 |
| **UART** | `decode1`（bus, PULL） | 1（`bus.cfg.core`） | plan `uart.rx` 1,024 + `chunk` 512（heap） | 同上 |
| **TCP（PULL）** | `decode0` | 0（`bus.cfg.core`） | plan `tcp.rx` 16,410 + `chunk` 8,192（heap） | 同上 |
| **TCP（PUSH）** | producer `tcp_nc4_rx`（NVS `rx_core`）→ consumer `decode0` | rx_core → 0 | **插槽 8 × 8,205 = 65,640（heap）** + parser plan 16,410 | 同上（frame-aligned） |
| **WS** | httpd task | httpd 建立時的核（我們沒指定，預設 0） | **BSS 24,576** | 直接 `nc4_parser_frame()`，不經 bus |
| **UDP** | `udp_task` | **寫死 1** | **BSS 16,384** | 直接 `nc4_parser_frame()` |

「可配置」的只有三樣：`bus.cfg.core`（code）、NVS `net/rx_core`（TCP producer）、NVS `net/rx_split`、`net/frame_align`。
WS 與 UDP 的核心不是宣告來的 —— WS 跟著 httpd、UDP 寫死 1。

---

## 2. 一個指令「執行完才能收下一個」嗎？

**對一半。**

- **handler 層：對，但範圍是「一個 decoder」。** 正確的模型是
  **一個 decoder 一次執行一個 handler；多個 decoder（每核一個）可以同時各跑一個**。
  `nc4.c` 的鎖已經從「全域」改成「屬於 decoder 物件」（2026-11-02），
  所以鎖的作用範圍＝引擎數，不是 CPU 數。
- **但今天只開了一個 decoder。** `main.c` 只 `nc4_decoder_create()` 一次，
  兩條 decode task 共用它 → 實際上仍是「全域一次一個」。這是「還沒拆」，不是設計。
  要拆成兩顆：`engines[]` 每個 `kind: decode` 一個 decoder、一份回覆緩衝，
  handler 必須 reentrant（不能共寫 static）。
- **接收層：不對。** 每條 bus 有自己的 parser/RX 區域，**另一核可以繼續收**。
  PUSH 模式更明顯：producer 把整幀寫進插槽（8 格），consumer 在忙時它們就排隊。
  所以「執行完才能收下一幀」只對**同一條 PULL bus 的 parser** 成立；有 ring 就可以吸收。

要不要更多緩衝，其實是問：**handler 忙的時候，你想排隊（更多插槽）還是丟幀（插槽滿就丟）**。
目前的設定是 TCP 8 格、WS/UDP 一條（訊息本身必須完整）。

---

## 2.5 CRC 與打包器：不屬於任何一核，兩核共用同一份

CRC 不是「某一層」的東西，它是**幀編解碼（frame codec）**的一部分，
和「解包」與「打包」綁在一起、在同一個檔案裡：

| 動作 | 函式 | 誰呼叫 |
|---|---|---|
| 打包（寫 CRC） | `nc4_pack_into()` | 任何核的 handler（回覆時）＋ benchmark 工具 |
| 解包（驗 CRC） | `nc4_parser_feed()` / `nc4_parser_frame()` | 每條 bus 的 decode task（在它被綁定的那一核） |

兩邊用同一個實作：`esp_rom_crc32_le(0, buf + 2, NC4_HDR_LEN - 2 + plen)`
（CRC 範圍從 `ver` 開始，**不含 SOF**）。

**所以「CRC 屬於哪一核」這個問題不成立**：它是**無狀態的純函數**，兩個核呼叫的是同一份 code，
不需要鎖、也不需要每核一份。真正有狀態的只有兩樣：

| 有狀態的東西 | 在哪 | 為什麼要獨立 |
|---|---|---|
| parser 實例（ring + 黏包狀態） | 每條 bus 一個 | 半個幀的殘留屬於**單一連線**，不能共用 |
| decoder（dispatch 表 + 鎖） | 全域一個 | 今天只開一個；要拆成每核一個就得處理 reentrancy（見 §2） |

**線 → 核 的綁定**：

- **設計**：router 指定（`system.json` 的 `nc4.routes[]`，每條 `in` 綁一個 `decodeN`）。
- **現況**：`nc4_bus_cfg_t.core` 直接寫死（CDC 0、UART 1、TCP 0），`routes[]` 未實作。

**回覆會從同一核出去**，這點現在已經成立而且不需要額外機制：
handler 在 decode task 裡執行，`ctx->send` 就是**那條 bus 的 send**，
所以「哪一核解、就哪一核回」是結構決定的，不是設定出來的。

---

## 2.6 要開雙核解碼／打包，要拆什麼

先分清三層狀態 —— **不是「每核一份緩衝」這麼簡單，是三種不同顆粒度**：

| 層 | 內容 | 幾份 | 為什麼 |
|---|---|---|---|
| ① **指令表**（catalog） | `cmd → handler` + owner | **一份，唯讀** | 開機註冊完就不再改；唯讀資料兩核共用安全 |
| ② **引擎**（engine） | 執行鎖、回覆 scratch、計數、cid / master_cid | **每核一份** | 這些是會被寫的狀態 |
| ③ **handler 的 static** | 各 handler 自己的 static 緩衝 | **每核一份**，或改成 reentrant | 真正的污染源 |

### 現況：哪一個共用會撞

| 東西 | 現在 | 兩核同時跑會怎樣 |
|---|---|---|
| `nc4_decoder_t.lock` | 一個 decoder 一把 | 兩核共用 → 退化成串行（不會壞，但失去雙核意義） |
| `reply_json()` 的回覆緩衝 | ✅ **已修**：直接砌入 TX 幀（stack，每任務私有） | 安全，已無共用 |
| `status` 的 JSON 緩衝（約 10 KB BSS） | 每個 key 一份 static | 同上 |
| `on_bus_stats` / `nc_task_ledger` 等的 `static char` | 各自一份 | 同上 |
| `nc4_pack_into()` | 純函數（`memmove`，支援就地打包） | ✅ 安全 |
| parser（每條 bus） | 每條一份 | ✅ 安全（前提見下） |

> ⚠️ **今天還不會撞**：所有 handler 都在同一把 decoder 鎖下執行（全域串行）。
> 上面那幾個 static 是「**不能開第二個 decoder**」的債，不是現在正在出錯的東西。
> 這一點要分清楚，否則會誤以為現在就有 race。

**關鍵**：`reply_json` 的 `frame[]` 在 **stack**（每次呼叫一份，安全），
但 `payload[]` 在 **static**（全系統一份，不安全）。

### 所以「用戶層限制不會同時執行同一條指令」不夠

因為兩核跑的是**不同**指令（例如一邊 `STATUS_GET`、一邊 `OTA_WRITE`），
它們還是會共用同一個 `reply_json` 的 static `payload[]`。

**最低要求**：

1. ✅ **已完成**：`reply_json` 不再需要獨立 scratch —— 回覆直接砌入 TX 幀的 DATA 區
   （`frame + NC4_HDR_LEN`），再就地呼叫 `nc4_pack_into()`（它用 `memmove`）。
   緩衝因此跟任務棧走，`static payload[2102]` 已刪，stack 用量不變（本來就有 `frame[2115]`）。
2. status 的 JSON 緩衝收斂成 engine 的一份（或共用一塊 + 那把鎖）；
3. `nc4_decoder_t` 拆成 **catalog（共用、唯讀）＋ engine（每核：鎖 + scratch）**；
4. 逐個 handler 審 static：有 static 的改成吃 engine scratch，或標記「只准 core0」。

### 一條 bus 只能有一個引擎服務

- 每條 bus 記「我歸哪個引擎」（`cfg.core` 現在 / 設計上是 `cfg.decoder`）；
- 換核 = **停 → `nc4_bus_reset()`（清半幀）→ 在另一核 start**，不能兩邊同時收；
- 這是「半幀不能交給另一核」的直接推論。

### 打包（TX）為什麼天然可以雙核

`nc4_pack_into()` 是純函數（只寫呼叫者給的緩衝）→ 兩核可同時打包。
只要「輸出緩衝」不是 static，就沒有共享狀態。現況 `TX_BENCH` 的分核模式
（core1 打包進 hub 插槽、core0 送出）已經在跑這條路。

---

## 3. 你「以為沒有、其實有」的緩衝

| 位置 | 大小 | 分類 | 備註 |
|---|---|---|---|
| TCP PUSH 插槽 | 8 × 8,205 = **65,640** | **heap**（`calloc`） | `rx_region` 只給 parser，插槽另配 —— 看 config 會誤會 |
| 每條 bus 的 `chunk` | TCP 8,192 / CDC 1,024 / UART 512 | heap | `read_chunk` |
| WS `s_rx` | 24,576 | BSS | 累積 fragment 後才解析 |
| UDP `s_rx` | 16,384 | BSS | 一則訊息 |
| `file_action` `s_tx` | 8,205 | BSS | 回覆 |
| `main.c` 的 status/JSON 緩衝 | 約 10,000 | BSS | 每個 key 一份 |
| `reply_json` | **2,115**（全部在 stack） | stack | 回覆直接砌入 TX 幀；decode stack = 6,144 的原因 |

**結論**：在「一個 decoder」的前提下，action 層不需要再配；但接收層的緩衝**已經比想像多**，
而且有一部分在 heap、`keys=mem` 看不到。一旦真的開兩個 decoder 並行，
每個 decoder 需要**自己的一份回覆緩衝** —— `reply_json` 這條已經解決（跟棧走），
剩下的債在 status / bus stats 的 BSS。

---

## 4. 要做的話，優先序

1. **插槽進 plan**（TCP 65,640）—— 它是最大一塊，而且現在完全不可見。做法：`nc4_bus_cfg_t` 改成吃「plan 給的 slots 區域」（像 parser 的 `rx_region`），PUSH 模式不再 `calloc`。
2. **WS / UDP 納入 bus 或至少納入計畫**：現在兩個 BSS 加起來 40 KB，core 也不是宣告來的。若要「配置解碼路線」，這兩個是缺口。
3. **status JSON 緩衝收斂**：10 KB BSS 換成一塊共用（dispatch 已互斥）。
4. 之後才是 `system.json` 的 `nc4.routes[].exec` —— 有上面三項，配置才有意義。
   路由表的完整設計見 [routing.md](routing.md)。

---

## 5. 一句話

**不需要為「指令」加緩衝；需要的是把「接收」的緩衝列管**（插槽 heap → plan、WS/UDP BSS →
宣告），以及決定每條通道能吸收幾幀。執行本身是全域串行的，所以回覆緩衝一份就夠。
