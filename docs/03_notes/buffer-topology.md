# 現在的緩衝結構（2026-11-11，0.1.2）

> 來自 A 板 `STATUS keys=mem`。`where S` = internal SRAM，`P` = PSRAM。
> `slot` = 單格 bytes；`n` = 格數；`stream` = parser 用的連續環。

## 1. `mem.buf` 清單

| 名稱 | where | slot | n | 用途 |
|---|---:|---:|---:|---|
| `TCP.slots` | P | 2048 | 4 | TCP PUSH slots（zero-copy recv 目的地） |
| `TCP.rx` | S | 8208 | 1 | TCP NC4 parser stream |
| `UDP.slots` | P | 2048 | 4 | UDP PUSH slots |
| `UDP.rx` | S | 8208 | 1 | UDP parser stream |
| `WS.slots` | P | 24576 | 2 | WS 訊息 slots（24 KB） |
| `WS.rx` | S | 24640 | 1 | WS parser stream |
| `CDC.slots` | S | 256 | 4 | CDC PUSH slots |
| `UART.slots` | S | 256 | 4 | UART PUSH slots |
| `nc4.reply0` | P | 2615 | 1 | NC4 回覆緩衝（core0） |
| `nc4.reply1` | P | 2615 | 1 | NC4 回覆緩衝（core1） |
| `nc4.rx` | S | 4192 | 1 | NC4 plan 的 RX stream |
| `uart.rx` | S | 1024 | 1 | UART RX plan |
| `ESPNOW.rx` | S | 64 | 1 | ESP-NOW 小訊息 |
| `probe.hub` | S | 512 | 1 | probe/hub |
| `fx.arena` | P | 524288 | 1 | effects arena |

- `mem.buf`：**14 個**，total **643,438 B**。
- plan：`int 5,728 B`、`psram 529,518 B`；network slots 是 runtime `nc_buf`，不經 plan。
- `STATUS sys h_int` 約 **80 KB**（0.1.2、開機後）。

## 2. 資料流

```text
TCP/UDP producer
  select() → nc4_bus_push_begin(slot) → recv() 直接入 slot
  → commit → kick → decode

WS producer
  xQueuePeek(訊息) → push_begin(slot) → pop/copy
  → commit → kick → decode

CDC producer
  TinyUSB callback notify → push_begin(256 B slot)
  → tinyusb read 入 slot → commit → kick（參考實作）

decode
  parser stream (TCP.rx / UDP.rx / WS.rx / ...)
  → nc4_decoder → handler／route
```

- CDC/UART 用 256 B×4；network 用 2 KB×4（WS 24 KB×2）。
- 所有 PUSH producer 都係「有資料才拿 slot」，不會揸住 slot 等 blocking read。
- parser stream 仍在 SRAM；下一步可以搬 PSRAM 或縮小。

## 3. 觀察

- network slots 搬 PSRAM 後，internal 由 10 KB 回到 ~80 KB。
- parser `TCP.rx/UDP.rx/WS.rx` 仍佔 internal（約 41 KB 合計）。
- `WS.slots 24 KB` 是為了 24 KB WS 訊息；如果只傳單幀可縮到 8 KB。
- `fx.arena 512 KB` 是最大單一 PSRAM buffer，和 network 無關。
