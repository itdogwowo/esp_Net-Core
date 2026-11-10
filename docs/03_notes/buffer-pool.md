# 共用緩衝池（nc_pool）與 PSRAM 實測

> 2026-11-09。對應 §7.29。這頁回答三件事：
> ① 為什麼要有共用 pool、它長什麼樣；② 跨核轉寄怎麼走；
> ③ PSRAM 到底夠不夠快（用這塊 S3 實測，不是估）。

## 1. 設計

三個固定 size class，全部由 `system.json` 的 `nc4.pool` 決定：

| class | 預設 | 用途 | 位置 |
|---|---|---|---|
| `small` | 1,024 × 8 | 一般命令（<1 KB） | internal |
| `bulk` | 4,112 × 3 | 大指令／OTA／FILE（4 KB） | internal（可 PSRAM） |
| `forward` | 4,112 × 2 | 跨核轉寄 staging | **必須 internal**（要交給 UART/TCP） |

- 每格是 **descriptor ＋ data**：descriptor 在 `.bss`（`nc_pool_slot_t`），data 在 class arena。
- `size` 會 **向上對齊 64 B**（4,112 → 4,160），DMA/cache line 友善。
- 狀態：`FREE → WRITING → READY → READING → FORWARD → SENDING → FREE`；
  `refcnt` 1 = 一個 owner，`retain/release` 支援多目的地／tap（未接 subscriber）。
- 執行緒安全：一個 `portMUX` 保護 free list 與 refcnt；臨界區只做指標操作，不阻塞。
- **不是** parser 狀態：每條線的 framing（半幀、SOF 重同步）仍然 per-link。

程式位置：

| 檔案 | 內容 |
|---|---|
| `components/sys/mem/include/nc_pool.h`、`nc_pool.c` | pool、descriptor、refcount、JSON |
| `main.c` `ph_pool()` | 由 `nc4.pool` 建立；開機階段 "pool"（declare 之前） |
| `nc4.c` `fwd_enqueue()`／`nc4_fwd_poll()`／`nc4_fwd_json()` | 跨核轉寄 queue |
| `nc4_bus.c` `nc4_bus_kick_core()` | 目的核 notify（**唯一允許的跨核 notify**） |
| `test_action` `0xF01C MEM_BENCH` | internal／PSRAM 頻寬實測 |

## 2. 設定

`system.json`：

```json
"nc4": {
  "pool": [
    { "name": "small",   "size": 1024, "count": 8, "where": "internal" },
    { "name": "bulk",    "size": 4112, "count": 3, "where": "internal" },
    { "name": "forward", "size": 4112, "count": 2, "where": "internal" }
  ]
}
```

- `where`: `internal` / `psram`；`forward` 用 PSRAM 會令 UART/TCP 的 `send` 拿到不可 DMA 的記憶體，**不要**。
- `count` 上限 32（descriptor 陣列 `NC_POOL_MAX_SLOTS`）；class 上限 4。
- 開機 log：`pool class small 8 × 1024 B（internal，合計 8 KB）`…、`pool init: ESP_OK`。
- `STATUS keys=mem` → `mem.pool`：

```json
{"init":1,"classes":[
  {"name":"small","size":1024,"count":8,"used":0,"peak":0,"fails":0},
  {"name":"bulk","size":4160,"count":3,"used":0,"peak":0,"fails":0},
  {"name":"forward","size":4160,"count":2,"used":0,"peak":1,"fails":0}]}
```

## 3. 跨核轉寄（**已被 comm-buffers.md 取代**）

> 2026-11-09 定案：forward 改用 PSRAM 的 `nc_hub`（per 目的 link × 來源核），
> `nc_pool.forward` 取消。以下保留作歷史；新設計見
> [comm-buffers.md](comm-buffers.md)。



```
來源核（decode0）                         目的核（decode1）
parser_forward()
  dst_core = nc4_router_core(dst)
  if dst_core != my_core:
    nc_pool_alloc(FORWARD) → copy → 入目的核 queue
    nc4_bus_kick_core(dst_core)  ──────► decode loop 每輪 nc4_fwd_poll()
                                          nc4_link_send(dst, slot)
                                          nc_pool_release(slot)
  else:
    nc4_link_send() 直接送（同核，唔經 queue）
```

- queue 深度 8／核；滿了 → `drops++`（整幀丟，不重試）。
- 跨核 notify 是 FreeRTOS `xTaskNotifyGive`（IPC 中斷）；**只給轉寄用**，bus producer 仍然同核 kick。
- `STATUS keys=net` → `net.fwd`：

```json
{"core0":{"frames":0,"drops":0,"err":0},"core1":{"frames":0,"drops":0,"err":0}}
```

- `frames` = 成功交出；`drops` = pool 冇格／queue 滿；`err` = `nc4_link_send` 失敗（例如 UART driver 未開）。

## 4. 上板結果（S3、tests 映像）

### 4.1 pool 建立

- 開機 `pool init: ESP_OK`；small 1024×8、bulk 4160×3、forward 4160×2。
- pool 本身 **28,992 B**（8×1024＋3×4160＋2×4160）＋ descriptor ~1.5 KB；
  上板 `sys.h_int` 237,403（tests 任務有跑）／253,983（USB storage 模式，部分測試任務未跑），
  兩個數都比 pool 前少 ~30 KB，符合。
- `STATUS keys=mem` 的 `mem.pool` 與上表一致；`fails=0`。

### 4.2 跨核轉寄（CDC → UART）

暫時把 `CDC` 的 route 改成 `["decode0","UART"]`（UART 在 core1），送 6 條 STATUS_GET：

| 指標 | 結果 |
|---|---|
| `net.fwd.core1.err` | **6** |
| `net.links.UART.drops` | **6** |
| `pool.forward.peak` | **1**（一次一幀，release 正常） |
| `pool.forward.fails` | **0** |
| `pool.forward.used`（查詢時） | 0 |

- 6 = 5 條 sys status ＋ 1 條 net status；queue／copy／notify／poll 全部走過。
- `err` 而非 `frames`，是因為這塊板 SD 佔用 GPIO7/8，UART driver 未開（`uart_send` 回 -1）；
  **轉寄機制本身已驗證**，成功路徑等 UART 腳位決定後再測。
- 測試後 `system.json` 已**逐位元組還原**（sha256 `086bbbd7…`），`net.fwd` 全 0。

### 4.3 PSRAM／internal 頻寬（`0xF01C`）

payload `[u16 kb][u16 iters]`；S3 tests、無 flash 活動、PSRAM 8 MB：

| 區域 | 大小 | memset | read（32-bit） | copy | 備註 |
|---|---:|---:|---:|---:|---|
| internal | 32 KB | **483–507 MB/s** | 42–51 MB/s | **133–252 MB/s** | 32 KB 在 cache 內 |
| PSRAM | **16 KB** | **212 MB/s** | 52 MB/s | **152 MB/s** | **cache-hot**（< 32 KB D-cache，8 次同一塊） |
| PSRAM | 64 KB | 29 MB/s | 30 MB/s | 20 MB/s | steady-state |
| PSRAM | 256 KB／1 MB | 33 MB/s | 32 MB/s | 21 MB/s | steady-state |

- `msync`（先 memset 弄髒再量）：16 KB **316 µs**、64 KB **521 µs**、256 KB **515 µs**
  → **約 0.3–0.5 ms，唔係同 buffer 大小成正比**（cache 只有 32 KB，flush 的是最後那段 dirty line）。
- **關鍵**：16 KB 的 PSRAM copy 是 152 MB/s，64 KB 以上跌到 20–21 MB/s。38.4 KB 剛好超過 32 KB
  cache，所以**整幀 streaming 要用 20–30 MB/s 預算**；如果切成 ≤16 KB 一段、用完即棄，
  可以食到 cache（~150 MB/s），但只限「同一段立即被 consumer 用完」。
- PSRAM 實測只有 **20–33 MB/s**（steady-state），不是理論 160 MB/s。

`0xF01D TILE_BENCH`（40 KB、8 次；每塊寫完立即 copy）：

| tile | full | tile | 倍率 |
|---:|---:|---:|---:|
| 2–16 KB | 12 MB/s | **16 MB/s** | 1.3× |
| 32 KB | 12 MB/s | 13 MB/s | 1.1× |

- 分塊只快 1.3×，不是 6×：**單次 streaming 是 bus-bound**；cache 只幫「同一塊重複使用」。
- 結論：與其追求 tile 大小，不如**減少 pass 數**；tile 用來 bound latency 同做 multi-pass 局部性。

### 4.4 對 38.4 KB LED frame（16×600×4）的可行性

| 動作（38.4 KB，整幀 streaming） | PSRAM | internal |
|---|---:|---:|
| 寫一幀（memset 等級） | 1.16–1.32 ms | 0.08 ms |
| 讀一幀 | 1.28 ms | 0.75 ms |
| copy 一幀 | **1.8–1.9 ms** | **0.15–0.3 ms** |
| cache flush（38.4 KB） | **~0.4–0.5 ms** | — |

- 60 fps 預算 16.7 ms：**一次 PSRAM copy = 11–12%**；加 flush ~3%；3 passes ≈ 33–36%。
- **重要修正（`0xF01D` 實測）**：16 KB 的 152 MB/s 是「同一塊重複 copy」的 cache-to-cache 數字；
  **新鮮的 40 KB frame 分塊 copy 只有 16 MB/s**（full 12 MB/s，tile 2–16 KB 都係 16）。
  因為每個 byte 最終都要寫入 PSRAM，tiling 唔會令單次 streaming 快過 bus。
  分塊真正的好處是：**bound latency**（每次 flush 9.6 KB ~0.1–0.2 ms）同 **multi-pass 局部性**
  （同一 tile 喺 cache 內做多次），唔係單 pass 的頻寬。
- 所以 PSRAM 的預算要按 **pass 數** 計：38.4 KB 寫一次 1.2 ms、讀一次 1.2 ms、flush 0.4 ms
  ≈ **2.8 ms（17% @60fps）**；每多一個全幀 read/write pass 就多 ~1.2 ms。
- 多 pass 效果應該 **copy 入 internal tile（≤16 KB）做，再寫回 PSRAM 一次**，唔好每 pass 都掃 PSRAM。
- I80 DMA 直接讀 PSRAM 的實際頻寬要等 I80 硬體；CPU copy 20–21 MB/s 是保守下限；
  分塊 tx_color（4–8 KB）同時把 msync 壓到 ~0.1–0.2 ms。

## 5. 限制與未做

| 項目 | 現況 |
|---|---|
| TTL／leak watchdog | 未做；slot 卡住只會令 `used` 上升，`STATUS` 看得到 |
| tap／第三方讀取 | refcount 已在，但未接 subscriber |
| bulk 放 PSRAM | 設定可選，未實測 |
| forward 成功路徑 | 等 UART 腳位（SD 衝突）或第二條可送出的 link |
| pool 用於 RX（header-first assembler） | 未接；現況仍用 per-link parser ring／PUSH slots |
| 多實例（UART0/1/2、I2C、SPI） | 未做 |
| 8 KB class（P4 TCP） | P4 config 的 `forward` 已設 8,208；未上板測 |

## 6. 決策紀錄

- **用固定 size class，不用 buddy／chain**：命令 <1 KB、大幀 4 KB，class 已覆蓋；冇 external fragmentation。
- **forward 每核一條 queue（深度 8），不用 per-destination**：同核快線 bypass；跨核才入 queue。
- **bulk 由 BEGIN/END claim**（未來接 OTA/FILE）；不是每幀 alloc。
- **PSRAM 只放資料，不放 DMA／forward**；forward 必須 internal。
- **跨核 notify 是例外**：只給 forward，bus producer 仍然同核（`nc4_bus_kick` 會計 remote kick）。

## 7. 量測指令

```bash
# pool 狀態
python3 tools/nc4.py --port /dev/cu.usbmodem* status mem,net
# PSRAM/internal 頻寬（256 KB、4 次）
python3 - <<'PY'
import struct,sys; sys.path.insert(0,'tools')
from nc4_demux import pack, Reader, ADDR_BROADCAST
import serial
s=serial.Serial('/dev/cu.usbmodem*',115200,timeout=0.2)  # 換成你的 port
s.reset_input_buffer()
s.write(pack(0xF01C, struct.pack('<HH',256,4), ADDR_BROADCAST))
r=Reader(s).wait_for({0xF004},20)
n=struct.unpack_from('<H',r[1])[0]; print(r[1][2:2+n].decode())
PY
```
