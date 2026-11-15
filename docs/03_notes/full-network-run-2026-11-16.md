# 全網絡測試（含流量）2026-11-16

## 測試範圍

- HTTP static
- UDP request/reply
- TCP request/reply
- TCP burst（流量壓力）
- WS handshake + request/reply + burst
- 嘗試 raw TCP 單向流量

> `net_bench.py` 的 request/reply 不是 raw 吞吐；真正的單向 MB/s 要用 raw endpoint。
> 今次 raw endpoint 沒有啟動（:5000/:5001 拒絕），所以 raw 單向流量標無效。

## S3（WiFi AX3000，10.10.1.31）

Reboot 後測試：

| 測試 | 結果 | 有效 |
|---|---:|---|
| HTTP ×10 | 10× 200 | ✅ |
| UDP ×50 | 133.9 replies/s，RTT p50 5.89 ms | ✅ |
| TCP ×100 | 122.9 replies/s，RTT p50 6.78 ms | ✅ |
| TCP burst 500 | 410.8 replies/s | ✅ |
| TCP burst 1000 | 411.1 replies/s | ✅ |
| WS ×100 | 107.5 replies/s，RTT p50 7.48 ms | ✅ |
| WS burst 300 | timeout | ❌ |
| WS burst 500 | timeout | ❌ |
| raw TCP :5000/:5001 | connection refused | ❌ |

之前另一次 reboot 後：TCP burst500 403.7/s、WS burst300 155.2/s、UDP50 136.4/s。
差異在 WiFi 排程/AP，不是 protocol。

## P4-C6（WiFi hosted C6，10.10.1.33）

| 測試 | 結果 | 有效 |
|---|---:|---|
| HTTP ×10 | 10× 200 | ✅ |
| UDP ×50 | 144.8 replies/s，RTT p50 5.53 ms | ✅ |
| TCP ×100 | 154.1 replies/s，RTT p50 5.46 ms | ✅ |
| TCP burst 500 | timeout | ❌ |
| WS ×100 | timeout | ❌ |
| WS burst 300 / 500 | timeout | ❌ |
| raw TCP | connection refused | ❌ |

Log：

```text
eh_sdio: SDIO RX read (2048 bytes) exceeds staging target (1536 bytes)
eh_sdio: rx_get_buffer(1590) failed; skipping read
```

## 失敗分類

| 現象 | 根因 |
|---|---|
| P4-C6 TCP burst／WS timeout | hosted SDIO RX staging 1536 < 2048 |
| S3 WS 300/500 timeout | WS session／burst 上限；100 OK |
| S3 之前 connected 但 ping 不到 | WiFi link state stale；reboot 回復 |
| raw endpoint 拒絕 | rawbench service 沒有啟動（config/實作未通） |
| HTTP 200 | 只證明短請求可用，不是 burst／流量 |

## 結論

- 今天「全部協議」都有跑：HTTP、UDP、TCP、WS、burst。
- TCP／UDP request/reply 在 S3 與 P4-C6 都有效。
- **S3 TCP burst 有效；WS 300/500 不穩。**
- **P4-C6 只有小量 request 有效；burst／WS 被 SDIO 1536 B staging 卡死。**
- **單向 raw 流量沒有有效數據**，因 raw endpoint 未啟動，不可用 `net_bench` 數字冒充 MB/s。

## 下一步

1. 修 P4-C6 SDIO staging（≥2048）。
2. 修 S3 WiFi health（connected ≠ reachable）與 WS burst。
3. 修 raw endpoint 啟動（config/初始化次序），才有真正單向流量。

## 追加：P4-C6 SDIO 4096 嘗試（2026-11-16 02:20）

- 把 `ESP_TRANSPORT_SDIO_MAX_BUF_SIZE` 由 1536 暫時改成 4096，重燒 P4-C6。
- 開機後：
  - ping 10.10.1.33：0% loss（RTT 14–66 ms）
  - HTTP `/`：200
- 再跑：
  - TCP burst 500：timeout
  - WS 100／300：timeout
  - UDP 50：timeout
- 即係拉高 host 的 `MAX_SDIO_BUFFER_SIZE` **沒有解決** hosted C6 的 burst 問題。
- 已把 macro 還原 1536，避免留下未驗證的 managed component 修改。
- 真正要查的是 CP 側 SDIO aggregate／rx staging target，而不是單純 host macro。

## 追加：S3 完整 matrix（reboot 後）

| 測試 | 結果 |
|---|---:|
| HTTP ×10 | 10× 200 |
| UDP ×50 | 133.9 replies/s，p50 5.89 ms |
| TCP ×100 | 122.9 replies/s，p50 6.78 ms |
| TCP burst 500 | 410.8/s |
| TCP burst 1000 | 411.1/s |
| WS ×100 | 107.5/s，p50 7.48 ms |
| WS burst 300 | timeout |
| WS burst 500 | timeout |

- S3 TCP burst 約 411/s 穩定。
- WS 100 可以，300/500 已經 timeout；不是 HTTP 問題。

## Raw 單向流量

今次 raw endpoint（:5000/:5001）仍然 connection refused，沒有新的 raw MB/s。
之前 S3 raw 收過：
- upload 0.41–0.57 MB/s
- download 2K 1.42 MB/s，4K 以上 timeout
但那是獨立一輪，不能和今次 request/reply 混在一起。
