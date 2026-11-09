# 發送／轉寄路徑的設計（2026-11-08 討論結論；同日已落地）

> **這份文件回答四個問題**（使用者原話整理）：
> 1. 回覆幀只有 2,615 B，轉寄要送 4 KB 夠不夠？
> 2. 回覆用 PSRAM 會不會造成中斷？
> 3. 發送要不要用 DMA／更大的緩衝？
> 4. 現在轉寄／發送是不是**一定要等送完**才能做其他事？
>
> **結論先講**：CDC 的回覆**不用等**（2,615 < 4 KB FIFO）；UART 現在**會發**（1 KB TX 環，
> 整幀阻塞到排空）；P4 TCP 的 8 KB 幀**會等**；**轉寄已經實作**（2026-11-08 晚，
> 走 `nc4_link` ＋ `routes[].out`，同步送、不經回覆幀）。要「解碼完就交出去」的做法是
> **目的通道自己的 TX 緩衝**（CDC FIFO／UART ring／TCP sndbuf／WS async），handler 只
> copy 進去就返回；塞不下時由該 transport 自己決定等多久。
> 大小按「目的通道的最大幀」決定，不是每條 bus 都配；S3 不需要 8 KB，8 KB 只發生在 P4 TCP。

---

## 1. 現況（2026-11-08 晚，可以逐項查證）

| 通道 | 發送函式 | 「交出去」的緩衝 | 大小 | 單幀上限 | 何時會等 | 實測 |
|---|---|---|---:|---:|---|---|
| **CDC** | `cdc_send` | TinyUSB CDC TX FIFO | **4,096 B** | 2,615 B（回覆上限） | 只有 FIFO 滿（主機不讀）→ 等 100 ms 後丟 | RTT 1.08–1.15 ms；20 條排隊 1.21–1.26 ms/條 |
| **UART** | `uart_send`（`uart_write_bytes`） | UART driver TX ring | **1,024 B** | 1,011 B（region 1,024） | 整幀排入環為止（4 KB 幀 ≈ 44 ms @921600） | 21 B 幀：`0xF01B` → `ESP_OK`，迴路收到 |
| **TCP（P4）** | `tcp_nc4_send` | lwIP socket send buffer | **5,760 B** | 8,205 B | **>5,760 B 就阻塞**，直到對端 ACK | 未量 |
| **轉寄** | `nc4_link_send` → 目的通道的 write | 目的通道 TX 緩衝 | 見上 | 見上 | 由 transport 決定（同上） | `CDC→UART`：21 B 幀轉寄成功、`drops=0` |

補充：
- 回覆 JSON 上限 `NC4_REPLY_JSON_MAX = 2,600`，幀 2,615。**超過不再靜默截斷**：
  `reply_send`／`reply_json` 會改回合法物件
  `{"err":"reply_too_big","need":N,"cap":2600}`（上板驗證：`0xF01A 3000`）。
- `CDC_TX_WAIT_US = 100 ms`：主機不讀時保護 decode 不卡死。
- 每核一個 decoder（§7.26）之後，**一核在等，另一核照跑**（已驗證：core1 卡 280 ms，core0 RTT 1.13 ms）。
- 跨核轉寄之後，CDC／TCP 多了一把「整幀 mutex」：兩個核同時送出時，分 chunk 寫會把兩幀切爛。
  log 路徑**唔攞**這把鎖（照用 `s_tx_reply` 的 critical section），所以 log 永遠唔會卡回覆。

## 2. 為什麼 CDC 今天不用等

`cdc_send` 只做「memcpy 進 4 KB FIFO ＋ flush」就返回（µs）。2.4 KB 的 STATUS 在 1 MB/s 下
線路要 2.4 ms，但那段時間**和下一個指令的處理重疊**——所以 20 條排隊是 1.21 ms/條
（瓶頸是 USB 每 1 ms 一個 transfer），不是 1.2＋2.4 ms。

> 這就是「C 版本交給硬件處理傳送」在 CDC 上的實際樣子：**FIFO 就是那個「放好的位置」**。

## 3. 大幀與轉寄：三種做法（選了第二種）

| 做法 | 資料誰持有 | decode 何時解放 | 成本 | 適用 |
|---|---|---|---|---|
| **同步送** | 來源 parser region | **送完才解放** | 0 RAM | UART 小幀（<1 KB） |
| **copy → 目的 ring**（**已採用**） | 目的通道 TX 緩衝 | **µs 後**（memcpy） | 緩衝本身 ＋ 1 次 copy | 一般回覆／轉寄 |
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
- 今天先開 UART TX **但唔擴環**：1 KB 已經覆蓋「命令／回覆」；4 KB 轉寄會阻塞 ~44 ms。
  要 decouple 才付 +3.2 KB（見 §9）。

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
  每核 decoder 之後這層保護消失 → 仍未另外處理（見 §9）。

## 7. 實作順序（已完成 1、3、4；2 只做了「開」，5 維持不做）

1. ✅ **`nc4_link` 出口表 ＋ `nc4_link_send(dest, ptr, len)`**：名字 → 目的通道的 write；
   整幀交出去，成功計 `frames/bytes`，短送／錯誤計 `drops`（**不能重試**：來源緩衝即時失效）。
2. ⚠️ **UART TX**：`uart_send` 已接上，**ring 仍然 1,024**（零 RAM）。4,192 的擴環未做。
3. ✅ **轉寄**：router 的 `out` 支援 link 名稱 → `parser_dispatch` 先轉寄、後本地 dispatch，
   **不經過回覆幀**。濾走 `decodeN`、自己、重複；開機做 link 稽核。
4. ✅ **回覆截斷修正**：超過 2,600 → 合法錯誤物件（不再靜默截斷）。
5. ✅ **不做** S3 的 8 KB 轉寄；8 KB 留給 P4 TCP（插槽已有）。

## 8. 量測（2026-11-08 晚，S3）

| # | 量什麼 | 結果 |
|---|---|---|
| 1 | 4 KB 轉寄時 decode 的實際佔用 | **未量**（沒有 4 KB 轉寄需求；UART 環 1 KB 先擋住） |
| 2 | P4 TCP 8 KB 幀的阻塞時間 | **未量**（P4 不在手邊） |
| 3 | OTA 抹除／寫入時 UART 串流掉多少幀 | **未量** |
| 4 | 大包發生率 | `nc4_link` 的 `bytes/frames` 已經有，但未分桶 |

已量到的（上板、S3、暫時 `SDIO=0` 讓 GPIO7 做 UART 迴路）：
- 直接 `0xF01B FWD UART`：`{"err":"ESP_OK","frames":3,"bytes":74,"drops":0}`，
  `LOOP_STAT rx` 由 0 → 2（連同 FWD 自己的幀被 router 轉出去）。
- router：`CDC out=["decode0","UART"]`，主機送 **1 條 21 B 的 PING** →
  `LOOP_STAT rx` +2（本地執行 1 次 ＋ UART 迴路回來再 1 次）。
- 統計：`links.UART frames 9 / bytes 175 / drops 0`；UART bus `frames 12, crc_err 0, bad_hdr 0`。
- 回覆超限：`0xF01A 3000` → `{"err":"reply_too_big","need":3000,"cap":2600}`。

## 9. 未決問題（更新）

1. ~~UART 要不要 TX？~~ → **要**（已接上，1 KB 環）。**要不要擴到 4,192（+3.2 KB）** 仍未決定；
   只有「要轉寄 4 KB 進 UART」才需要。
2. **轉寄滿了怎麼辦？** 今天沿用各 transport 的語義（CDC 100 ms 後丟、UART 排空、TCP ACK）。
   要改成「立即丟＋計數」就必須為每個 transport 提供非阻塞寫入（CDC 的 FIFO 可以，
   TCP 要 `MSG_DONTWAIT` ＋ 續傳）。
3. ~~8 KB 轉寄要不要納入需求？~~ → **不要**（S3 永遠不配 8 KB）。
4. ~~共享 staging 還是每 link ring？~~ → 用各 transport 自己的 TX 緩衝（等於每 link 一個），
   冇額外共享 staging。
5. **拓撲成環**：`CDC→UART` 與 `UART→CDC` 兩條互指目前未擋（自我反射已濾）。
   要嘛開機靜態檢查、要嘛幀頭加 TTL——NC4 表頭冇 TTL 欄位。
6. **OTA flash 停頓 vs UART**：每核 decoder 之後，OTA（core0）停 cache 時 core1 的 uart.rx
   冇保護。仍未處理。

## 10. 程式碼索引（用符號名，避免行號漂移）

| 位置 | 內容 |
|---|---|
| `main.c` `cdc_send()` | FIFO 迴圈 ＋ 100 ms deadline ＋ 整幀 mutex |
| `main.c` `uart_send()`／`uart_link_write()` | 整幀 `uart_write_bytes`（未安裝 driver 回 -1） |
| `main.c` `tcp_nc4_send()`／`tcp_link_write()` | `send()` 迴圈 ＋ 整幀 mutex，出錯直接斷線 |
| `main.c` `reply_send()`／`reply_json()` | 超過 2,600 → `{"err":"reply_too_big",…}` |
| `nc4_link.c` | 名字 → write fn；`nc4_link_send`、統計、JSON |
| `nc4_router.c` `nc4_router_links()` | 從 `out` 濾出真正的轉寄目的地 |
| `nc4.c` `parser_forward()` | 先轉寄、後 ADDR 過濾與 dispatch |
| `nc4.c` `parser_dispatch()` | handler 執行期間 parser 環被佔（不變量來源） |
| [status §7.26](../00_pm/status.md) | 每核 decoder（發送的並行性前提） |
| [status §7.28](../00_pm/status.md) | 本次落地的完整記錄與上板數據 |
