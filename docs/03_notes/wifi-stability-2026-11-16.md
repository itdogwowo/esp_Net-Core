# WiFi 測試失敗分析（2026-11-16）

## 為什麼 HTTP 過、其他測試失敗

HTTP `/` 只是一個很短的 static request；它只證明「當下 TCP 可以建立、HTTP handler 回得到」，不代表 WiFi/SDIO 在 burst 下穩定。

## P4-C6

- WiFi hosted C6，經 SDIO。
- Log：
  ```text
  eh_sdio: SDIO RX read (2048 bytes) exceeds staging target (1536 bytes)
  eh_sdio: rx_get_buffer(1590) failed; skipping read
  ```
- 1536 B staging 不夠裝 2048 B aggregate → RX 停頓 → TCP burst／WS／UDP timeout。
- 這是 hosted WiFi 的 SDIO staging buffer 問題，不是 NC4 line。
- 詳見 `p4c6-wireless-test-2026-11-16.md`。

## S3

### 失敗模式 A：status 說 connected，但 IP 不可達

```text
STATUS wifi: connected=1, ip=10.10.1.31, rssi=-45
ping 10.10.1.31: Host is down
HTTP: 000
```

- WiFi state 是 stale：driver 仍報 connected，但 L2/ARP 已經沒有流量。
- Reboot S3 後回復：
  ```text
  ping: 0% loss
  HTTP: 200
  ```
- 這是 link health 問題，不是 NC4 decoder。

### Reboot 後有效數據

| 測試 | 結果 |
|---|---:|
| TCP burst 500 | **403.7/s** |
| WS burst 300 | **155.2/s** |
| WS burst 500 | timeout |
| UDP 50 | **136.4/s**，RTT p50 5.8 ms |
| HTTP / | 200 |

- WS 300 穩定；500 會 timeout，和之前一致。
- UDP／TCP 在 reboot 後都正常，證明 timeout 不是永久 protocol 壞掉。

## 結論

1. **P4-C6**：SDIO RX staging 1536 B < 2048 B，burst 必失敗。
2. **S3**：WiFi 可能 stale connected；測試失敗後要先 reboot，不要直接當 protocol 失敗。
3. **WS 500**：S3 session／burst 上限；300 穩定。
4. HTTP 成功不能當成 network 穩定的證據。

## 測試流程要加

```text
測試前：ping + HTTP health check
失敗後：reboot / 重連，再跑，不把 stale failure 當數據
每個 burst：記錄 RSSI、heap、是否 timeout
```
