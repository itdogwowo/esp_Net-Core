# 統一網絡線路：TCP／UDP／WS → `nc4_bus`

> 2026-11-11。**性質：S3 第一版已上板；P4 之後接同一層。**
> 目的：三條線在 NC4 層共用同一組 `read()`／`send()`，而且跟 UART／CDC 一樣是
> **PUSH producer → slots → kick decode**；不再用 PULL 輪詢。效能數字見 §4。

---

## 1. 架構

```text
TCP / UDP / WS adapter
   │  read(buf, cap) → bytes / 一則訊息
   │  send(buf, len) → 同一條線
   ▼
nc4_bus（PUSH slots，每條線一個 parser、獨立黏包狀態）
   ▼
nc4_decoder → handler → 回覆走同一條 send
```

- `nc4_line_net.c` 建三個 `nc4_bus`（`PUSH`）：
  - `TCP`：listen 6000、single client、blocking `recv`。
  - `UDP`：bind 6001、blocking `recvfrom`，回覆到最後來源。
  - `WS`：httpd 8080 `/nc4`、訊息 queue，`httpd_ws_send_frame_async`。
- 每條線一個 producer task：`nc4_bus_push_begin` → blocking read 直接寫入 slot
  → `push_commit` → `nc4_bus_kick`。**跟 UART／CDC 完全同一種緩衝策略。**
- 三條都註冊 `nc4_link_register("TCP"/"UDP"/"WS")`；
  route 不需要特別寫，parser 會本地解碼，回覆走 link。
- `NC4_MAX_BUSES` 4 → **8**（S3：CDC／UART＋TCP／UDP／WS）。
- WS 需要 `CONFIG_HTTPD_WS_SUPPORT=y`。

啟動次序：

```text
ph_commit   nc_wifi_boot()
             → nc4_line_net_declare(decoder, core0)   // 建 bus，socket 未開
ph_channels nc4_line_net_start()                     // bind 6000/6001/8080
```

`ph_declare` 不能在 commit 前做，因為那時 `nc_wifi_used()` 還是 false。

---

## 2. 為甚麼改成 PUSH

- UART／CDC 是 PUSH：producer 讀 driver → slots → kick decode。
- 網絡第一版用 PULL，RTT 被 decode loop 的 10 ms 粒度拖住；而且 burst 會 reset。
- 現在三條線都改成 PUSH producer（同 UART），所以：
  - 同一套 slot／kick／parser 路徑；
  - decode 不再輪詢 socket；
  - 有資料才 kick，idle 時 producer 阻塞在 `recv`／queue。
- 代價：每條線多一個 4 KB stack 的 producer task；slot 記憶體也要預留。

---

## 3. 統一了什麼／沒統一什麼

| 已統一 | 說明 |
|---|---|
| NC4 framing | 三條線同一個 `nc4_parser` 路徑 |
| read/write 介面 | `int read(buf,cap)` / `esp_err_t send(buf,len)` |
| link 出口 | 全部 `nc4_link_register`，同一轉寄／回覆路徑 |
| core 歸屬 | `nc4_bus_cfg.core` 決定由哪個 decode loop 服務 |
| 緩衝策略 | 三條線和 UART／CDC 一樣：PUSH slots → kick decode |
| 狀態 | `STATUS net` 的 `bus`／`links` 同一份 |

| 未統一 | 說明 |
|---|---|
| socket 生命週期 | TCP accept／UDP peer／WS client 各自管 |
| 訊息邊界 | UDP/WS 是訊息；TCP 是 stream（由 parser 處理） |
| 背壓 | PUSH slots 滿時 producer 會 `vTaskDelay(1)`；burst 仍可能丟幀，未做優先級調校 |

---

## 4. 上板效能（A 板、WiFi、S3）

`python3 tools/net_bench.py --ip <device-ip> --mode ... --count 20`

| 線 | RTT p50 | p95 | min | max | reply/s（一問一答） |
|---|---:|---:|---:|---:|---:|
| TCP 6000 | **8.70 ms** | 80.51 | 6.60 | 102.11 | 57.4 |
| UDP 6001 | **8.60 ms** | 66.48 | 6.20 | 127.64 | 51.3 |
| WS 8080 | **16.75 ms** | 23.40 | 12.80 | 31.87 | 56.3 |

- PUSH 之後 p50 由 ~12 ms 降到 **8.6–8.7 ms**（TCP/UDP）；WS 由 21.4 → 16.8 ms。
- p95 仍高（66–80 ms），反映 WiFi／排程抖動；不是穩定低延遲。
- **burst 100 幀** 還未在 PUSH 版重測；之前 PULL 版會 reset。這一版仍只當「統一＋可用」，
  不是高吞吐定案。
- 下一步：producer priority／slot 數／budget 調校，再重測 burst。

### 4.1 流量對比（window 2、20 幀、`STATUS sys`）

| 線 | frames/s | payload KB/s | req B | rep B | 備註 |
|---|---:|---:|---:|---:|---|
| TCP | 55.0 | **14.13** | 360 | 3,340 | 有 ACK／Nagle 以外 overhead |
| UDP | **141.1** | **28.36** | 360 | 3,340 | 小封包最快 |
| WS | 56.1 | 11.17 | 360 | 3,340 | httpd／WS frame overhead |

- 這是 **NC4 payload**（request＋reply frame），不是 IP/TCP/UDP/WS wire bytes；wire 會再多 header。
- 20 幀、window=2；不是 burst。burst 100 仍會 reset（PULL 版觀察），PUSH 版未重測。
- UDP 的 frames/s 明顯高，因為沒有 TCP ACK／WS framing；但 UDP 沒有重傳，掉包就是丟幀。

---


### 4.2 完整一發多收測試（2026-11-11）

100 幀（4096 B 為 50 幀），payload 512／1024／4096；用 `0xF040` 內部 seq 統計，
`tools/net_oneway.py` 會把「收到數量不對」或「內部時間算出 >20 MB/s（不可能）」標 invalid。

**通過（frames 全到、lost=0、速度合理）：**

| 線 | payload | batch | 結果 | 有效速度 |
|---|---:|---:|---:|---:|
| TCP | 1024 | 10 | 100/100, lost 0 | **2.17 MB/s** |
| WS | 1024 | 10 | 100/100, lost 0 | **1.75 MB/s** |
| WS | 4096 | 4 | 50/50, lost 0 | **7.26 MB/s** |
| UDP | 512 | 2 + 5 ms pacing | 100/100, lost 0 | **0.147 MB/s** |

**不通過／invalid（不可當速度）：**

| 線 | payload | 現象 | 判斷 |
|---|---:|---|---|
| TCP | 512 | 100/100，但內部算 120 MB/s | timer／queue 污染，invalid |
| TCP | 4096 | 39/50 | 未收齊，invalid |
| WS | 512 | 110/100 | 殘留／重複，invalid |
| UDP | 512 | 72/100, lost 18 | 無 pacing 真掉包 |
| UDP | 1024 | 100/100，但算 34 MB/s | timer 污染，invalid |

**結論：**

- 50 KB/s 是「一幀一次 send」的 overhead；batching 後有效速度到 **MB/s 級**。
- 但完整矩陣仍未全過；TCP 4096、WS 512、UDP 1024 要修 queue／timer／殘留後重測。
- `net_oneway.py` 的 `ok` 已經把「frames 不對」和「速度不可能」都當 fail，唔會再靜靜報大數。
- 現階段最可信：TCP 1024 ≈ **2.17 MB/s**、WS 4096 ≈ **7.26 MB/s**、UDP 512 paced ≈ **147 KB/s**。
- 這不是最終 WiFi 上限；仲有 lwIP／producer／queue 調整空間。

### 4.3 PSRAM 修正（已做一半）

- `nc4_bus` 新增 `slots_psram`；network 三條線的 PUSH slots 已搬 PSRAM：
  `TCP.slots P`、`UDP.slots P`、`WS.slots P`。
- `STATUS sys h_int` 由 **10.5 KB → 68 KB**（A 板，剛開機）。
- parser `TCP.rx／UDP.rx／WS.rx` **仍然 S**（8,208 B ×3），下一步再搬 PSRAM 或縮小。
- lwIP 預設窗口／mailbox 太細，已調：
  `TCP_WND/SND_BUF=65535`、`TCP/UDP_RECVMBOX=64`。
- WiFi `WIFI_PS_NONE`（測速時關 modem sleep）。
- 這個改動是 buffer manager 直接促成的：`mem.buf` 一眼看到 57 KB internal 不見了。

### 4.4 路由測試（未完成）

測試用 routes 曾加過：

```json
{"in":"TCP","out":["decode0","CDC"]},
{"in":"CDC","out":["decode0","TCP"]}
```

但 network 路徑不穩，**未完成可重現的 network→CDC／CDC→TCP 驗證**；
repo 的 `system.json` 已還原成原本 4 條 routes，避免把測試 forwarding 帶入正式設定。

### 4.5 天線

A 板 RSSI = **-47 dBm**（TestAP，距離近）。這個訊號已經很好；換天線不是 50 KB/s 的主因。
若你要試外置天線，先量同一位置的 RSSI／retry；除非 RSSI 掉到 -70 以下或 retry 明顯，
不然先做 batching／buffer 調校的收益更大。


### 4.6 零複製 producer（參考 CDC）與路由測試

**零複製 producer（已完成）**：

TCP/UDP 改成同 CDC 一樣：

```text
select(fd, 100ms) → 有資料 → nc4_bus_push_begin() → recv() 直接入 slot
→ commit → kick
```

- 不會再「揸住 slot 等 blocking read」，result／下一幀不會被卡住。
- WS 用 `xQueuePeek` 等訊息，拿到 slot 後才 pop／copy。
- 好處：少一次 copy、少 staging buffer、abort spin 大減；`h_int` 約 80 KB。

**路由測試（PASS，2026-11-11）**：

一次只開一個方向，避免上一版同時開造成的 CDC TX／decode 卡死。

| 方向 | 測試 | 結果 |
|---|---|---|
| TCP → CDC | TCP client 送 `STATUS_GET`，USB 收到 forwarded frame | ✅ |
| UDP → CDC | UDP 送，USB 收到 | ✅ |
| WS → CDC | WS 送，USB 收到 | ✅ |
| CDC → TCP | USB 送，TCP client 收到 forwarded frame | ✅ |
| CDC → UDP | USB 送，UDP client 收到 | ✅ |
| CDC → WS | USB 送，WS client 收到（需先完成握手／prime） | ✅ |

- 測試 routes 用完已還原成原本 4 條；repo `system.json` 沒有測試 forwarding。
- **注意**：network→CDC 與 CDC→network 同時開，曾令 A 板 NC4 無回應；
  正式合併 routing 前一定要再做雙向壓力測試。

## 5. 測試工具## 5. 測試工具

```bash
python3 tools/net_bench.py --ip <device-ip> --mode tcp --count 50
python3 tools/net_bench.py --ip <device-ip> --mode udp --count 50
python3 tools/net_bench.py --ip <device-ip> --mode ws  --count 20
```

- 預設一問一答 RTT；`--burst` 先全送再收（現階段可能 reset）。
- 需要 `websocket-client` 套件給 WS。
- 裝置 IP 在 `STATUS keys=wifi` 的 `ip_str`。

---

## 5.1 緩衝管理器：有用，但唔係免費

`nc_buf` **有實際用到**，不是只做報表：

```json
"TCP.slots": {"where":"S","slot":2048,"n":4}
"TCP.rx":    {"where":"S","slot":8205,"n":1,"stream":1}
"UDP.slots": {"where":"S","slot":2048,"n":4}
"UDP.rx":    ...
"WS.slots":  {"where":"S","slot":8205,"n":2}
"WS.rx":     ...
```

`STATUS keys=mem` 的 `mem.buf` 共 14 個 buffer；network 三條線的 slots＋parser 約 **57 KB internal**。
加上其他 plan／runtime buffer，`STATUS sys` 的 `h_int` 只剩 **約 23 KB** —— 這是目前最危險的數字。

- 好處：所有 buffer 有名、有 owner、有 fill／gen／abort／xcopy 可看；
  `TCP.rx`／`UDP.rx`／`WS.rx` 都是 `stream:1`，`copied` 每幀 18 B。
- 限制：`nc_buf` **不會自動搬去 PSRAM**；`nc4_bus` 的 slots 仍寫死 `NC_BUF_SRAM`。
- 副作用：`TCP.slots` 的 `abort` 已到 **182,061**（producer 冇 client 時 still push_begin→read 0→abort）。
  CDC 也有 91,223；呢個係 CPU／buffer churn，不是 leak，但唔靚。
- 結論：**buffer manager 有用（特別是揭露 23 KB 這個問題）**，但下一步要：
  1. 給 `nc4_bus` 一個「slots／parser 用 PSRAM」的選項；
  2. 或縮小 WS slot（8205 → 4096）；
  3. 修 TCP no-client 的 abort spin（等 client event 才 push_begin）。

## 6. 未完成

| 項目 | 狀態 |
|---|---|
| P4 接同一層（ETH） | ⏳ 下一輪 |
| PUSH producer／低延遲 | ✅ 已接（TCP/UDP/WS）；burst 仍待調校 |
| TCP 多 client、UDP 多 peer | ⏳ 目前單一 |
| WS 多 client | ⏳ 目前單一 |
| TLS／認證 | ⏳ 未做 |
| route 自動化／peer registry | ⏳ 未做 |
| 舊 `nc4_udp`／`nc4_ws`（P4 path） | ⏳ 之後收斂到 `nc4_line_net` |
