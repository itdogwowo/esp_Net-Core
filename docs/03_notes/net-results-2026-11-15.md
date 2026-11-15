# 網絡效能：最終數據集（2026-11-15）

> 這是「一次過」整理後的最終紀錄。測試方法見 `net-test-plan.md`，
> 逐輪失敗／無效原因見 `net-test-matrix-2026-11-15.md`。

## 0. 量測類型（不可混比）

| 類型 | 工具 | 量什麼 |
|---|---|---|
| request/reply | `tools/net_bench.py` | STATUS 一問一答，回覆/s、RTT |
| 單向 NC4 | `tools/netbench_one.py`（0xF040） | payload 連續送，device timer，MB/s |
| raw TCP | `tools/raw_tcp_bench.py` / `raw_tcp_dual.py` | 繞過 NC4，socket MB/s |

## 1. 環境

| | P4 | S3 |
|---|---|---|
| Board | ESP32-P4-ETH | ESP32-S3-N16R8 |
| Flash／PSRAM | 16 MB／32 MB | 16 MB／8 MB |
| 網絡 | ETH 10.10.1.50 | WiFi STA AX3000 10.10.1.31，RSSI ~ -70 dBm |
| firmware | shipment（測試結果來自 test build） | shipment |
| 共同 line profile | TCP 2×8205, UDP 1×8205, WS 2×8192, mem auto | 同左 |
| 高頻 WS 建議 | WS 2×16384（讓 1K batch10、2K batch7、4K batch3 放得下） | 同左 |

## 2. Request/reply（共同 profile）

| Board | TCP RTT | TCP burst | WS RTT | WS burst | UDP RTT |
|---|---:|---:|---:|---:|---:|
| P4 | 3.5–4.0 ms | 5.7–7.0k/s | **3.6 ms** | ~1.0k/s | 3.2 ms |
| S3 | 7.4–8.7 ms | 950–1,000/s | **7.4 ms** | 266/s | 7–8 ms |

> WS RTT 是套用 `TCP_NODELAY`＋小回覆 single-send＋32-bit unmask 之後的數字；
> 舊版 P4 6.5 ms、S3 14–16 ms。小包延遲已接近 TCP。

\* S3 WS 500 burst 會 timeout；300 穩定，重連要 1–2 秒 pause。

## 3. P4 單向 NC4（公平 batch）

測試條件：TCP／WS 用同一 batch；WS slot 16384（1K/2K/4K），8K slot 16384 亦可。
單位：MB/s。

| Payload | batch | TCP | WS | WS/TCP |
|---|---:|---:|---:|---:|
| 1,024 B | 10 | 5.29–5.59（中位 5.51） | 4.77–5.41（中位 5.40） | ~98% |
| 2,048 B | 7 | 5.53 | 5.83 | ~105% |
| 4,096 B | 3 | 5.68 | 5.71 | ~100% |
| 8,000 B | 1 | 5.26 | 5.15 | ~98% |

- **WS 和 TCP 已經在同一水平（±5%）**；之前的 1K WS 1.87 是 batch=1 造成，不是 WS 上限。
- 要 1K batch10 需要 WS slot **16384**；8192 只可 batch7（1K），當時 WS 4.80 vs TCP 5.48。
- 細包要追上 TCP，關鍵是 **batch 多個 NC4 frame 在同一個 WS message**，不是加插槽數量。
- 舊的 batch 不公平表已由本表取代。

## 4. P4 raw TCP（單 stream，4K cap，source SRAM）

| 方向 | 2K | 4K | 8K | 16K |
|---|---:|---:|---:|---:|
| PC→device | 8.59 | 8.53 | 8.83 | 8.51 |
| device→PC | 8.15 | 8.19 | 8.05 | 7.94 |

raw-only（唔開 HTTP／NC4）：單 stream upload 8.52、download 7.83（4K）；
同 normal build 接近，證明 HTTP／line 閒置時不是主因。

- source task core1 prio12、SRAM 4K、SO_SNDBUF 32768。
- 舊 MPY raw：PC→device 5.72、device→PC 9.35。
- 所以 C PC→device 已勝；device→PC 仍低 ~12–14%。

## 5. P4 raw TCP（雙 stream，raw-only，8 MB，3 次）

| Run | Upload | Download |
|---|---:|---:|
| 1 | 9.572 | 9.426 |
| 2 | 9.162 | 9.634 |
| 3 | 8.929 | 9.574 |
| 平均 | **9.221** | **9.545** |
| 最高 | 9.572 | 9.634 |

- 兩條 stream，sink 5000/5003、src 5001/5002；每 core 一條；4K cap。
- 對比單 stream（8.2–8.5）：雙 stream 提升約 **8–16%**。
- 對比舊 MPY 9.35：C download 9.55 已超過；C upload 9.22 仍略低。
- 之前單次 11.787 是 **主機量測／endpoint 未穩定下的產物**，已作廢，不列入 baseline。

## 6. S3 raw TCP

| 方向 | 2K | 4K | 8K | 16K |
|---|---:|---:|---:|---:|
| PC→device | 0.58 | 1.33* | 0.54 | 0.32 |
| device→PC | timeout | timeout | timeout | timeout |

\* 4K 1.33 是單次 outlier；其餘 0.3–0.7。
raw-only 雙 stream：上載 0.44–0.46，下載 0（timeout）。
舊 MPY S3：上載 ~0.83、下載 ~0.80；C 仍未穩定超過。

## 7. Buffer／memory 對照

| Board | 設定 | TCP burst | WS burst | Buffer |
|---|---|---:|---:|---:|
| P4 | 1×8205，SRAM | ~6.8–7.0k/s | ~1.3k/s | ~49 KB |
| P4 | 2/1/2，auto/PSRAM | 6.96k/s | 1.03k/s | ~80 KB |
| S3 | 1×8205，PSRAM | ~1.0k/s | ~0.35k/s | ~49 KB |
| S3 | 2/1/2，SRAM | 不穩 | WiFi 斷線 | 內部 RAM 剩 1.4 KB |

- 1 個 TCP slot 在 8K burst 會 lost 493/500；**TCP 最少 2 slots**。
- WS 1K 細包慢；要 batch 大 frame 就要加大 WS slot，否則 8K slot 會斷線。
- PSRAM 本身不會斷線；S3 SRAM 2 slots 才是內部 RAM 耗盡的原因。

## 8. 舊 MPY 歷史基準（不同 firmware）

| P4 路徑 | 2K | 4K | 8K | 16K | 峰值 |
|---|---:|---:|---:|---:|---:|
| raw TCP 上載 | 8.99 | **9.35** | 7.65 | 4.75 | 9.35 |
| raw TCP 下載 | 3.22 | 3.22 | 3.25 | 5.13 | 5.72 |
| NC4 fast pack | 5.68 | 6.48 | 7.02 | **7.37** | 7.37 |

| S3 路徑 | 2K | 4K | 8K | 峰值 |
|---|---:|---:|---:|---:|
| raw TCP 上載 | 0.74 | 0.81 | **0.83** | 0.83 |
| raw TCP 下載 | 0.67 | 0.74 | 0.79 | 0.80 |
| NC4 fast pack | 0.65 | 0.71 | 0.72 | 0.74 |

## 9. 結論（给性能规划用）

1. **單 TCP stream 的 C raw 約 8.2–8.8 MB/s**；MPY 舊 firmware 9.35 是歷史基準。
2. **P4 雙 stream aggregate 穩定約 9.2–9.6 MB/s**；download 已超過 MPY 9.35，upload 仍略低。
   雙 stream 只提升 8–16%，不是 double；100 Mbps PHY 是共同上限。
3. **NC4 協議本身約 5.5 MB/s**，低於 raw；這是 parser／handler 成本，不是網卡。
4. **WS 細包固定成本高**：1K 1.87 MB/s，8K 4.60 MB/s；要吞吐就要大 frame。
5. **PSRAM vs SRAM**：速度差不多；SRAM source 略快；混合 buffer 是合理方向，但未實作兩層 ring。
6. **S3 WiFi 上限 ~0.8 MB/s**；C 仍未穩定超過 MPY。

## 10. 無效／未完成（不得當數據）

- P4 raw 雙 stream 11.787（舊量測，已作廢；穩定版是 9.2–9.6）
- S3 device→PC raw 全部 timeout
- S3 0xF040 單向（test build 沒回覆）
- S3 WS 500 burst timeout（300 穩定）
- P4 大 buffer 8×TCP / 4×WS 單次 3.70 MB/s（未重複）

## 11. 目前實機狀態

- P4 `10.10.1.50`：shipment firmware，HTTP 200
- S3 `10.10.1.31`：shipment firmware，STA AX3000，HTTP 200
- raw endpoint 已 config gate off
- 共同 profile：TCP 2×8205、UDP 1×8205、WS 2×8192、mem auto
