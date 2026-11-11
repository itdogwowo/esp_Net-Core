# 連線行為：什麼時候連、什麼時候開 AP、什麼時候重試

> 2026-11-11。**這一頁是規則，不是描述**：改 `nc_wifi.c` 之前先對這張表。
> 對齊對象：舊專案 `mp_Net-Core/slave/lib/sys/network_manager.py`。

---

## 1. 開機：只試一輪，失敗就停

```text
wifi.enable = 0            → 不初始化，0 B
wifi.enable = 1
  ├─ ssid 有值 → **直接連它**（不掃描、不輪憑證清單）
  └─ ssid 空   → 掃描一次（只是為了對名字）→ 對得上憑證清單的就連（RSSI 由強到弱）
                 **掃不到的憑證：不盲連**（2026-11-11 使用者定案）
  全部失敗 → **停手**（不再掃描、不再重試）
```

規則三句：

1. **寫了 ssid 就照寫的連**，不掃描。掃描不是連線前置動作。
2. **沒寫 ssid 才掃描**，而且只用「掃到嘅 SSID 對得上憑證」嘅組合去連。
3. **掃不到就唔連**（不盲連）。掃描喙其他用途只是「顯示給用戶看」（`0xF038` 的配對畫面）。

**為什麼要停手**：之前是「失敗 → 等 30 秒 → 再掃一次 → 再盲連」，實測每 ~36 秒一次。
每一次 scan/connect 都會把 channel 鎖住幾百 ms，**連已經連上的 AP 都會被拖慢**，
量吞吐時看起來像 buffer 不夠（2026-11-11 的 P4→S3 測試就被這個污染）。

對齊舊專案：舊版的 `_init_wifi()` 是「STA 失敗 → 起 AP ＋ 保留 STA」，
但**不會**在背景無限重連；重連是由 `check_network()` 依模式決定的。


## 2. 執行期斷線：不自動重連

`WIFI_EVENT_STA_DISCONNECTED` 只更新狀態與 log，**不呼叫 `esp_wifi_connect()`**。
要重連是外部動作（目前是 `0xF02C WIFI_STA` 指令；將來可以是由 STATE 管理器觸發）。

理由：自動重連會跟 AP 模式、手動切換、OTA 互相打架，而且症狀是「偶爾慢」這種最難查的東西。

## 3. 什麼時候開 AP

| 情況 | 行為 |
|---|---|
| `network.json` `wifi.mode = "ap"` | 開機直接起 AP（不做 STA） |
| `wifi.mode = "apsta"` | 兩者都起 |
| `wifi.mode = "sta"`（預設）＋ 連不上 | **目前不會自動開 AP** —— 這是待辦，舊專案會 |
| 執行期 `0xF02D WIFI_AP` | 立即起 AP（測試用；重開機消失） |

AP 的預設值（舊專案）：`NetCore-<cID>` / `12345678`。
Net-Core 目前只在測試指令裡指定 ssid/pw，**還沒有**「連不上就開 AP」的退路。

> 待辦：把「STA 一輪失敗 → 起 AP（NetCore-<cID>）→ 停在 AP」寫進 `nc_wifi.c`，
> 這樣現場沒有 AP 時仍然進得去（用手機連它）。要不要做、什麼條件做，見 §5。

### 3.1 配置形態（與舊專案同一個 schema，只在你啟用 Wi-Fi 之後才生效）

```jsonc
// network.json
"wifi": {
  "enable": 0,            // ★ 唯一前提：0 = 唔初始化、唔佔 RAM、下面全部唔讀
  "active_mode": 1,       // 0 = off｜1 = always on｜2 = boot only（用完 timeout 就關）
  "timeout": 300,         // 只對 active_mode=2 有意義（秒）
  "ssid": "",             // 空 = 用 NVS 憑證清單（最多 10 組）；有值 = 直接連它（掃描只係顯示用，唔輪清單）
  "ssid_pw": "",
  "mdns_name": "",        // 結尾 "-" = 自動接 <cID>
  "ap_ssid": "",          // 空 = "NetCore-<cID>"（連不上時開的 AP）
  "ap_password": ""       // 空 = "12345678"（少於 8 字元就當冇設）
}
```

**沒有 `mode: sta|ap|apsta`** —— 舊專案也沒有。原因就是你講的：流程本身已經決定模式：

```
enable=0            → 什麼都不做
enable=1            → STA 試一輪
                      ├─ 成功 → 用 STA（active_mode=2 時，用完 timeout 關掉）
                      └─ 失敗 → 開 AP（ap_ssid/ap_password）並停在 AP
```

`active_mode` 只管「開機後這個介面要不要留著」，不是「用 STA 還是 AP」。

## 3.2 通用 vs 板商：哪個設定放哪裡

| 類別 | 例子 | 放哪 | 為什麼 |
|---|---|---|---|
| **通用行為** | `enable`／`active_mode`／`timeout`／`ssid`／`ap_ssid` | `network.json`（runtime） | 每塊板都一樣，改完重開就生效 |
| **板商差異** | ETH 的 `mdc/mdio/ref_clk`、UART 的 `tx/rx`、SPI 腳位 | `ports/<board>/wiring.json`（build-time）＋ `/data/config/hardware.json` 覆蓋 | 同一顆晶片，不同廠商接法不同 —— 不可能有「通用腳位」 |
| **能力碎片** | 有冇 ETH／WiFi 走 hosted／WS | `ports/_profiles/*.defaults`（board.json profiles） | 決定要不要編進 image |



## 4. 憑證

- 存在 NVS，最多 10 組（`wifi-credentials.md`）。
- 開機只用「已存在」的憑證，**不會**自己掃描配對新的（掃描配對是 `0xF038` 的動作）。
- 舊專案同一個思路：SSID 已連上就沿用，不重連、不掃描。

## 5. 未決

1. 連不上時要不要自動開 AP？（舊專案：要）—— 要就需要一個「AP 失敗再回 STA」的退出條件，否則會卡在 AP。
2. `wifi.timeout_s` 現在只在 boot-only 模式用；要不要像舊專案一樣「AP 模式下 N 秒後關 AP」？
3. 多組憑證之間要不要做 backoff（例如失敗的憑證 5 分鐘內不再試）？
