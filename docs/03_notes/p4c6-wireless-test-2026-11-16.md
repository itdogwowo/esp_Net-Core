# P4-C6 無線測試（2026-11-16）

## 環境

- Board：ESP32-P4-C6（P4 + 外掛 ESP32-C6，WiFi hosted）
- IP：`10.10.1.33`（STA AX3000）
- RSSI：約 -57 dBm
- firmware：P4-C6 shipment，新 netline schema
- services：
  - `wifi_tcp0 :80` → HTTP static
  - `wifi_tcp1 :6000` → NC4 TCP
  - `wifi_tcp2 :8080` → NC4 WS
  - `wifi_udp0 :6001` → NC4 UDP

## 結果

| 測試 | 結果 | 有效 |
|---|---:|---|
| HTTP `/` | 200 | ✅ |
| TCP :6000 順序 100 | 101.9 replies/s，RTT p50 6.82 ms、p95 18.98 ms、max 110 ms | ✅ |
| TCP :6000 burst 500 | timeout | ❌ |
| TCP :6000 再跑 300 | timeout | ❌ |
| WS :8080 | handshake 失敗／timeout | ❌ |
| UDP :6001 | timeout | ❌ |
| HTTP 之後 | 10.10.1.33 完全不可達 | ❌ |

## Log 觀察

```text
netline: [wifi_tcp1] tcp 連線
netline: [wifi_tcp2] WS handshake 失敗
eh_sdio: SDIO RX read (2048 bytes) exceeds staging target (1536 bytes)
eh_sdio: rx_get_buffer(1590) failed; skipping read
heap: 294 KB → 244 KB
```

## 判斷

- P4-C6 的 WiFi 是 hosted C6，經 SDIO；log 顯示 **SDIO RX staging buffer 不足**：
  - 2048 bytes 超過 1536 staging target；
  - rx_get_buffer(1590) failed。
- 之後 heap 下降、WS handshake 失敗、服務不可達。
- 不是 NC4 line／HTTP 問題；是 hosted WiFi 的 SDIO RX buffer 設定。
- TCP 100 次順序成功，但 burst 已經觸發不穩。

## 無效數據

- TCP burst 500、WS、UDP 全部 timeout，不列入 baseline。
- P4-C6 無線目前只能確認：
  - WiFi 可連 AX3000；
  - HTTP static 200；
  - TCP 順序小量 request 可用；
  - 不能作穩定吞吐測試。

## 下一步

1. 調 `esp_hosted` SDIO RX staging／buffer（1536 → 至少 2048+）。
2. 再看 heap 下降是否來自 SDIO queue。
3. 穩定後才重跑 P4-C6 TCP/WS/UDP burst。
