# 發送／轉寄路徑的設計（2026-11-08 討論結論，未實作）

> **這份文件回答四個問題**（使用者原話整理）：
> 1. 回覆幀只有 2,615 B，轉寄要送 4 KB 夠不夠？
> 2. 回覆用 PSRAM 會不會造成中斷？
> 3. 發送要不要用 DMA／更大的緩衝？
> 4. 現在轉寄／發送是不是**一定要等送完**才能做其他事？
>
> **結論先講**：今天的 CDC 回覆**不用等**（2,615 < 4 KB FIFO）；UART 根本不發；
> P4 TCP 的 8 KB 幀**會等**；**轉寄根本還沒實作**。要「解碼完就交出去」的唯一方法是
> **目的通道自己擁有一段 ring，handler 只 copy 進去（µs）就返回**。
> 大小按「目的通道的最大幀」決定，不是每條 bus 都配；S3 不需要 8 KB，8 KB 只發生在 P4 TCP。

---

## 1. 現況（可以逐項查證，2026-11-08）

| 通道 | 發送函式 | 「交出去」的緩衝 | 大小 | 單幀上限 | 何時會等 | 實測 |
|---|---|---|---:|---:|---|---|
| **CDC** | [`cdc_send`](../../firmware/main/main.c#L752) | TinyUSB CDC TX FIFO | **4,096 B** | 2,615 B（回覆上限） | 只有 FIFO 滿（主機不讀）→ 等 100 ms 後丟 | RTT 1.08–1.15 ms；20 條排隊 1.21–1.26 ms/條 |
| **UART** | `NULL`（沒有 send） | UART driver TX ring | 1,024 B | 1,011 B（region 1,024） | —（不發） | — |
| **TCP（P4）** | [`tcp_nc4_send`](../../firmware/main/main.c#L2914) | lwIP socket send buffer | **5,760 B** | 8,205 B | **>5,760 B 就阻塞**，直到對端 ACK | 未量 |
| **轉寄** | **不存在** | — | — | — | — | router 只做 `line → core`（[nc4_router.c](../../firmware/components/sys/nc4/nc4_router.c)） |

補充：
- 回覆 JSON 上限 `NC4_REPLY_JSON_MAX = 2,600`，幀 2,615。**超過會靜默截斷**
  （[main.c:845](../../firmware/main/main.c#L845) `reply_send`、[main.c:872](../../firmware/main/main.c#L872) `reply_json`）——
  這是缺陷，不是設計。
- `CDC_TX_WAIT_US = 100 ms`（[main.c:747](../../firmware/main/main.c#L747)）：主機不讀時保護 decode 不卡死。
- 每核一個 decoder（§7.26）之後，**一核在等，另一核照跑**（已驗證：core1 卡 280 ms，core0 RTT 1.13 ms）。

## 2. 為什麼 CDC 今天不用等

`cdc_send` 只做「memcpy 進 4 KB FIFO ＋ flush」就返回（µs）。2.4 KB 的 STATUS 在 1 MB/s 下
線路要 2.4 ms，但那段時間**和下一個指令的處理重疊**——所以 20 條排隊是 1.21 ms/條
（瓶頸是 USB 每 1 ms 一個 transfer），不是 1.2＋2.4 ms。

> 這就是「C 版本交給硬件處理傳送」在 CDC 上的實際樣子：**FIFO 就是那個「放好的位置」**。

## 3. 大幀與轉寄：三種做法（選一個）

| 做法 | 資料誰持有 | decode 何時解放 | 成本 | 適用 |
|---|---|---|---|---|
| **同步送** | 來源 parser region | **送完才解放** | 0 RAM | UART 小幀（<1 KB） |
| **copy → 目的 ring**（建議） | 目的通道 ring | **µs 後**（memcpy） | ring bytes ＋ 1 次 copy | 一般回覆／轉寄 |
| **pin 插槽**（零複製） | PUSH-frame 插槽 | 立即，但插槽被佔到送完 | 插槽 bytes（每格 8,205） | P4 TCP 大流 |

**關鍵不變量**：`frame.data` 指向 parser 環，handler 一返回就會被下一幀覆寫。
所以要嘛送完才返回、要嘛 copy 出去、要嘛把插槽 pin 住——**沒有第四種**。
「分三批送」只改變 CPU 讓出的粒度，不會解放流水線（parser 環仍然被引用）。

## 4. 每個通道要多大（按需求，不是每條 bus 都配）

| 目標 | 來源 region | 目的緩衝 | 額外 RAM | 線路時間 |
|---|---|---|---:|---:|
| **CDC 4 KB** | 4,192 ✅ | FIFO 4,096 ✅ | **0** | 4 ms（背景） |
| **UART 4 KB** | 1,024 → 4,192（+3.1 KB） | ring 1,024 → 4,192（+3.2 KB，必須內部） | **+6.3 KB** | 44 ms（背景） |
| **UART 8 KB** | 8,205（+7.2 KB） | ring 8,192（+7.2 KB） | +14.4 KB | 89 ms |
| **TCP 8 KB（P4）** | 16,410 ✅ | 8,205 插槽 ＋ sndbuf 5,760（會分段） | **0** | ~1 ms（100 Mbps） |

- 目前**沒有 8 KB 轉寄的產品需求**：命令是小包、OTA 是 4 KB、只有 P4 的 TCP 會到 8 KB（已經有）。
- 省 RAM 的替代：大包罕見 → **一個共享 4 KB staging ＋ mutex**（同一時間只有一個大包在飛），
  代價是大包會排隊。

## 5. DMA 的事實（IDF 6.0.3）

| 通道 | TX DMA | 實際機制 | 備註 |
|---|---|---|---|
| UART | **沒有**（`uart.h` 無 DMA API；UHCI 不是通用隊列） | ring ＋ ISR 排空 | ISR 每次搬 1 KB，µs 級 |
| CDC（FS device） | 沒有 | TinyUSB 4 KB FIFO | 效果等同「交出去就返回」 |
| ETH（P4） | **有**（EMAC GDMA） | pbuf → DMA | 這條才是真 DMA |

**DMA 不減少緩衝需求**：它需要一段穩定、連續的記憶體到送完為止——那正是 ring／插槽。
所以「分批 DMA」＝把 ring 切段餵 DMA，ring 本身仍然存在。

## 6. PSRAM 會不會造成中斷？（回覆幀在 PSRAM）

- 回覆幀在 PSRAM **不會**關中斷、不經 DMA，只走 cache（慢 ns/B 級）。實測 RTT 1.08–1.15 ms。
- **真正的中斷來源是 flash 寫入**：IDF 的 `spi_flash_write` 會停 cache，並用 IPC 把**另一核釘住**
  （`spi_flash_disable_interrupts_caches_and_other_cpu`），直到寫完。OTA 開始的整區抹除最長。
  - 對 UART：RX FIFO 只有 128 B ≈ 1.4 ms @921600，停頓超過就掉 byte（1 KB RX 環可多吸 ~11 ms）。
  - 對 CDC：USB 主機重送（NAK），不掉，只延遲。
  - 這與回覆幀放哪裡無關（整個核心被停）。
- ⚠️ 以前共用一把 decoder 鎖時，OTA 在 core0 寫 flash、core1 的 handler 剛好被鎖住（**意外保護**）；
  每核 decoder 之後這層保護消失 → 需要另外處理（見 §8）。

## 7. 建議的實作順序（下次照這個做）

1. **`link_send(dest, ptr, len)`**：`len ≤ ring 剩餘空間` → memcpy ＋ kick，**返回 0（不等）**；
   否則兩選一：**背壓**（等排空）或 **丟棄 ＋ 計數**（policy 可配）。
2. **UART TX ring 1,024 → 4,192**（要開 UART send 時才做；+3.2 KB 內部 RAM）。
3. **轉寄**：router 的 `out` 支援 link 名稱 → 走 `link_send`（copy），**不經過回覆幀**。
4. **回覆截斷修正**：`reply_send`／`reply_json` 超過 2,600 → 回明確錯誤（或分頁），不要靜默截斷。
5. **不做** S3 的 8 KB 轉寄；8 KB 留給 P4 TCP（插槽已有）。

## 8. 待驗證的量測（下次開工先跑）

| # | 量什麼 | 預期 | 方法 |
|---|---|---|---|
| 1 | 4 KB 轉寄時 decode 的實際佔用 | µs 級（copy 進 ring） | 在 handler 前後讀 `esp_timer`，`STATUS` 回報 |
| 2 | P4 TCP 8 KB 幀的阻塞時間 | ~1–8 ms（受 RTT／窗口） | 對端故意慢讀，量 `send()` 時間 |
| 3 | OTA 抹除／寫入時 UART 串流掉多少幀 | 可能有掉（FIFO 1.4 ms） | 串流中做 OTA，比對 `rx`／`lost` |
| 4 | 大包發生率 | 罕見（status／轉寄） | 在 `link_send` 加桶計數（<256／<1K／<4K／<8K） |

## 9. 未決問題（要使用者決定）

1. **UART 要不要 TX？** 現在 `send = NULL`，UART 只收不回。
2. **轉寄滿了怎麼辦？** 背壓（慢但可靠）還是丟棄＋計數（快但會掉）？
3. **8 KB 轉寄要不要納入需求？** 若不要，S3 可以永遠不配 8 KB。
4. **共享 staging 還是每 link ring？** 共享省 RAM 但大包會排隊。

## 10. 程式碼索引

| 位置 | 內容 |
|---|---|
| [`main.c:752`](../../firmware/main/main.c#L752) | `cdc_send`：FIFO 迴圈 ＋ 100 ms deadline |
| [`main.c:2914`](../../firmware/main/main.c#L2914) | `tcp_nc4_send`：`send()` 迴圈，出錯直接斷線 |
| [`main.c:845`](../../firmware/main/main.c#L845)／[`872`](../../firmware/main/main.c#L872) | 回覆長度 clamp（靜默截斷） |
| [`nc4_router.c`](../../firmware/components/sys/nc4/nc4_router.c) | 只有 `line → core`，沒有轉寄 |
| [`nc4.c`](../../firmware/components/sys/nc4/nc4.c) | `parser_dispatch`：handler 執行期間 parser 環被佔 |
| [status §7.26](../00_pm/status.md) | 每核 decoder（發送的並行性前提） |
| [status §7.1 D](../00_pm/status.md) | `out` 的 link 轉發「未做」 |
