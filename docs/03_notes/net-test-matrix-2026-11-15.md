# 網絡測試矩陣與方法紀錄（2026-11-15）

> 目的：把「用什麼方法、什麼 firmware／config、量到什麼」寫清楚，避免將
> request/reply、單向 NC4、raw TCP 三種數字混在一起。
>
> 相關：`net-bench-2026-11-15.md`（各輪細節）、
> 舊專案 `mp_Net-Core/doc/01_protocol/08_performance_benchmark.md`。

## 0. 先定義三種「速度」

| 指標 | 工具 | 意思 | 典型單位 |
|---|---|---|---|
| Request/reply | `tools/net_bench.py` | 送一個 STATUS，等一個回覆；`--burst` 先送 N 個再收 | 回覆/s、RTT ms |
| 單向 NC4 | `tools/netbench_one.py`（0xF040） | 連續送 N 個 NC4 資料幀，device 內部計 frames／bytes／時間 | MB/s |
| Raw TCP | `tools/raw_tcp_bench.py` | 繞過 NC4／line 層，直接 socket 灌 bytes | MB/s |

**同一句「7,000/s」不等於 7 MB/s**：
- 7,000 個 STATUS/s × 約 100 B ≈ 0.7 MB/s。
- 8–10 MB/s 只可能出現在 raw TCP 或大 frame 單向測試。

## 1. 硬件與 firmware

| | P4 | S3 |
|---|---|---|
| Board | ESP32-P4-ETH | ESP32-S3-N16R8（新換） |
| Flash／PSRAM | 16 MB／32 MB | 16 MB／8 MB |
| 網絡 | ETH 10.10.1.50 | WiFi STA `AX3000` 10.10.1.31，RSSI 約 -70 dBm |
| 測試 firmware | `--tests`；背景 NET_AUTO／ESP-NOW 停用 | 同左 |
| 正常 firmware | shipment，已還原 | shipment，已還原 |

共同 line buffer（test build 與 shipment 都用）：

```json
TCP 2 × 8205, UDP 1 × 8205, WS 2 × 8192, mem = "auto"
```

`auto` 在兩塊板實際都用 PSRAM；約 80 KB configured buffer。

## 2. 測試方法（可復現）

### 2.1 Request/reply

```sh
python3 tools/net_bench.py --ip 10.10.1.50 --mode tcp --count 100
python3 tools/net_bench.py --ip 10.10.1.50 --mode ws  --count 100
python3 tools/net_bench.py --ip 10.10.1.50 --mode tcp --count 1000 --burst
```

- 每次一個 STATUS_GET；`--burst` 先送 N 個再收。
- 量到的是回覆數／s；payload 細，不能當 MB/s。

### 2.2 單向 NC4（0xF040）

```sh
python3 tools/netbench_one.py --ip 10.10.1.50 --mode tcp \
    --count 500 --payload 8000 --batch 1 --timeout 10
```

- 需要 `--tests` firmware（0xF040 handler）。
- `payload` 是每幀 NC4 payload；8,000 B 是安全上限（8,192 會超過 8,205 frame）。
- WS 的 batch 是「一則 WS 訊息幾個 NC4 幀」；batch 後總長不可超過 WS slot（8,192）。
- device 回傳 `frames/bytes/us/lost`；`speed_mbps` 用 device timer 計算。

### 2.3 Raw TCP

```sh
python3 tools/raw_tcp_bench.py --ip 10.10.1.50 --total 4194304
```

- 上載：PC → device :5000；下載：PC → device :5001 送 `GO <bytes>`，device 回送。
- 新 firmware 的 raw endpoint 由 `network.json` 的 `"rawbench": 1` 開啟；
  **不需要 `NC_ENABLE_TESTS`**，避免 test build 的 flag 傳遞問題。
- source 每次 send ≤4 KB；source buffer 用內部 SRAM（對照舊 MPY 的上載 buffer）。
- 量到的是 raw TCP，不經 NC4／line 層；才可以和舊 MPY raw 9.35 比較。

## 3. 舊專案基準（歷史數據）

來源：`mp_Net-Core/doc/01_protocol/08_performance_benchmark.md`（2026-08-19，
MicroPython，P4 RMII／S3 WiFi）。這是舊 firmware／舊 protocol，不是今次 C firmware。

### P4

| 路徑 | 2K | 4K | 8K | 16K | 32K | 峰值 |
|---|---:|---:|---:|---:|---:|---:|
| 裸 TCP 上載（分段 ≤4K） | 8.99 | **9.35** | 7.65 | 4.75 | 4.09 | 9.35 @4K |
| 裸 TCP 下載 | 3.22 | 3.22 | 3.25 | 5.13 | **5.72** | 5.72 @32K |
| NC4 fast pack 上載 | 5.68 | 6.48 | 7.02 | **7.37** | 7.33 | 7.37 @16K |

### S3

| 路徑 | 2K | 4K | 8K | 16K | 32K | 峰值 |
|---|---:|---:|---:|---:|---:|---:|
| 裸 TCP 上載 | 0.74 | 0.81 | **0.83** | 0.82 | 0.81 | 0.83 @8K |
| 裸 TCP 下載 | 0.67 | 0.74 | 0.79 | 0.78 | **0.80** | 0.80 @32K |
| NC4 fast pack 上載 | 0.65 | 0.71 | 0.72 | **0.74** | 0.73 | 0.74 @16K |

舊專案 key facts：

- `SEND_CAP = 4096`：lwIP `TCP_SND_BUF` 約 4–5.7 KB；一次 send 大過就會阻塞。
- `RX_BUF_SIZE = 4115`：一個滿載 FILE_CHUNK（9+2+4+4096+4）。
- `MAX_PAYLOAD = 8192`。
- 上載 buffer 用 SRAM；下載大 buffer 用 PSRAM；`AtomicStreamHub` 預設 slots 在內部 SRAM。

## 4. 今次 C firmware 結果

### 4.1 Request/reply（共同 profile）

| Board | TCP RTT | TCP burst | WS RTT | WS burst | UDP RTT |
|---|---:|---:|---:|---:|---:|
| P4 | 3.5–4.0 ms | 6.8–7.0k/s | 6.5 ms | ~1.0k/s | 3.2 ms |
| S3 | 7.4–8.7 ms | 950–1,000/s | 14–16 ms | 300–350/s* | 7–8 ms |

\* S3 WS 連續重連會偶爾 timeout；加 1–2 秒 pause 後穩定。

### 4.2 P4 單向 NC4（8,000 B payload，batch 10，背景 test tasks 停用）

| Payload | 幀數 | TCP | WS |
|---:|---:|---:|---:|
| 1,024 B | 4,000 | 5.42 MB/s | 1.87 MB/s |
| 2,048 B | 2,000 | 5.08 MB/s | 2.91 MB/s |
| 4,096 B | 1,000 | 5.61 MB/s | 3.90 MB/s |
| 8,000 B | 500 | 5.55 MB/s | 4.60 MB/s |

- TCP 在 5.1–5.6 MB/s 平台，唔係包太細。
- WS 每則訊息固定成本明顯；越大 frame 越好。
- 8–10 MB/s **沒有在 NC4 路徑重現**；舊 9.35 是 raw TCP。

### 4.3 舊 vs 新 driver（P4，8K 單向）

| Driver | TCP | WS |
|---|---:|---:|
| 舊（PULL／httpd WS） | 5.42–5.59 MB/s | ~5.15 MB/s |
| 新（PUSH／自寫 WS） | ~5.62 MB/s | 4.65 MB/s |

新 driver request/reply 延遲明顯較好；單向大 frame 差不多。

### 4.4 PSRAM vs SRAM（P4，8K 單向）

| mem | TCP | WS |
|---|---:|---:|
| PSRAM | 5.62 MB/s | 4.65 MB/s |
| SRAM | 5.56 MB/s | 4.71 MB/s |

差異在量測噪聲內；PSRAM 本身沒有造成斷線。

### 4.5 最少 buffer（request/reply；1×8205 各線）

| Board | mem | TCP burst | WS burst | 備註 |
|---|---|---:|---:|---|
| P4 | SRAM | ~6.8–7.0k/s | ~1.3k/s | request/reply 最快、最省 |
| P4 | PSRAM | 6.8k/s | ~1.0k/s | 同級 |
| S3 | PSRAM | ~0.97–1.08k/s | ~0.32–0.37k/s | 安全 |
| S3 | SRAM | 988/s | 320/s | 1 slot 可，2 slots 會餓死 WiFi |

**但 8K 單向 burst 不能用 1 個 TCP slot**：P4 1×SRAM TCP 只收到 7/500 幀（493 lost）。
所以「最少 buffer」要分場景：

- request/reply／細指令：1 slot 已夠。
- 8K burst：TCP 最少 2 slots；WS 2 slots；UDP 1 slot。

### 4.6 最終公約數

```text
TCP 2 × 8205
UDP 1 × 8205
WS  2 × 8192
mem: auto
```

P4：TCP burst 6,961/s、WS 1,027/s、HTTP 200。
S3：TCP 950/s、WS 302/s、HTTP 200。

### 4.7 C raw TCP（4K send cap，4 MB，source/SRAM，source core1 prio 12）

| Board / 方向 | 2K | 4K | 8K | 16K |
|---|---:|---:|---:|---:|
| P4 上載（PC→device sink） | 7.43 | **8.53** | **8.83** | 7.57 |
| P4 下載（device src→PC） | **8.15** | **8.19** | 8.05 | 7.94 |
| S3 上載（PC→device sink） | 0.57 | 0.71 | 0.63 | 0.69 |
| S3 下載（device src→PC） | timeout | timeout | timeout | timeout |

調優內容：

- raw source 由 unpinned prio 5 改成 **core1、prio 12**
- source buffer 由 PSRAM 改成 **內部 SRAM**
- source 每次 send ≤4 KB（對齊舊 MPY `SEND_CAP`）
- `SO_SNDBUF = 32768`

對照舊 MPY raw：

| 方向 | 舊 MPY | 今次 C | 判斷 |
|---|---:|---:|---|
| P4 PC→device | 5.72（舊「下載」） | **8.15–8.53** | **C 較好** |
| P4 device→PC | 9.35（舊「上載」） | **8.05–8.19** | **C 仍低約 12–14%** |
| S3 PC→device | 0.80 | 0.57–0.71 | C 仍較差 |
| S3 device→PC | 0.83 | timeout | 未完成 |

所以「C 一定唔可以差過 MPY」**仍未完全達到**。已把 P4 上載由 ~6.9 提升到 ~8.8，
下載提升到 ~8.2；但 device→PC 仍低於舊 MPY 9.35。差異不是 protocol，而是
source task／host receive／S3 WiFi 排程，仍需再調。

## 5. 無效／未完成測試（必須標記，不可當數據）

| 測試 | 狀態 | 原因 |
|---|---|---|
| S3 單向 0xF040 | **無效** | test build 的 0xF040 在 S3 沒有回覆 |
| S3 raw TCP 下載 | timeout | 2K 以上全部 timeout；上載有數據 |
| P4 device→PC vs MPY 9.35 | 仍低 12–14% | source/host 未調到最佳 |
| 8–10 MB/s 重現 | 未重現 | 舊數字是 raw TCP；今次 NC4 最高 5.6 |
| S3 WS 500 burst | 不穩 | 300 穩定；500 會 timeout，與 session 清理／WiFi 有關 |
| P4 大 buffer 8×TCP / 4×WS | 單次 3.70 MB/s | 可能是 backpressure／量測噪聲，未重複確認 |

## 6. 如何避免下一次「數字對不上」

每次報告必須同時寫：

1. **量的是哪一種**：request/reply、單向 NC4、raw TCP。
2. **payload／batch／send cap**。
3. **firmware**：shipment 或 `--tests`；test 背景任務有冇停。
4. **buffer config**：slots／slot_size／mem。
5. **舊 driver 或新 line 層**。
6. **有效樣本數**：重複 2–3 次，失敗要標無效，不可取最佳值假裝成功。
7. **工具版本／命令**：`tools/net_bench.py`、`tools/netbench_one.py`、`tools/raw_tcp_bench.py`。

## 7. 結論

- **8–10 MB/s 是舊 raw TCP 的數字**；NC4 舊 firmware 最高 7.37，今次 C firmware 約 5.6。
- 今次 TCP 1K–8K 都約 5.1–5.6 MB/s，唔係「包太小」。
- WS 細包明顯慢（1K 1.87），大包改善（8K 4.60）。
- PSRAM 不是斷線元兇；S3 SRAM 2 slots 才是把內部 RAM 食光的原因。
- 最小可靠 buffer：TCP 2、UDP 1、WS 2；約 80 KB。
- 下一步要修好 raw TCP／S3 0xF040 test endpoint，才能公平比較「舊 raw 9.35 vs 新 raw」。
