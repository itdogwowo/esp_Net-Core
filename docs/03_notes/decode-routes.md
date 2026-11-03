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

## 3. 你「以為沒有、其實有」的緩衝

| 位置 | 大小 | 分類 | 備註 |
|---|---|---|---|
| TCP PUSH 插槽 | 8 × 8,205 = **65,640** | **heap**（`calloc`） | `rx_region` 只給 parser，插槽另配 —— 看 config 會誤會 |
| 每條 bus 的 `chunk` | TCP 8,192 / CDC 1,024 / UART 512 | heap | `read_chunk` |
| WS `s_rx` | 24,576 | BSS | 累積 fragment 後才解析 |
| UDP `s_rx` | 16,384 | BSS | 一則訊息 |
| `file_action` `s_tx` | 8,205 | BSS | 回覆 |
| `main.c` 的 status/JSON 緩衝 | 約 10,000 | BSS | 每個 key 一份 |
| `reply_json` | 2,102 static + 2,115 stack | BSS + stack | decode stack = 6,144 的原因 |

**結論**：在「一個 decoder」的前提下，action 層不需要再配；但接收層的緩衝**已經比想像多**，
而且有一部分在 heap、`keys=mem` 看不到。一旦真的開兩個 decoder 並行，
每個 decoder 需要**自己的一份回覆緩衝**（否則兩個 handler 會互踩 static）。

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
