# 失敗處理政策（config / 網路 / USB / 開機）

> **原則**：**任何單一失敗都不能讓裝置變磚**。
> 失敗要「看得見（log/status）、可回復、逐項隔離」。
> **狀態**：2026-11-02，部分已實作（見每節的 ✅/⬜）

---

## 1. 失敗分類與政策

| 類別 | 例子 | 偵測 | 政策 | 現況 |
|---|---|---|---|---|
| **設定檔** | 檔案不見、JSON 壞、值超範圍 | 開機載入 / HTTP PUT | 用預設值、逐欄位回報、**不覆蓋舊檔** | 🟡 讀寫已通、載入未做 |
| **硬體** | PHY autoneg 失敗、ETH 不存在 | 開機初始化 | 重試（autoneg）；仍失敗 → 不啟用該通道 | ✅ |
| **鏈路** | 網路線拔掉、switch 重開 | ETH event | 關閉連線、停止 accept；link up 後重建 | 🟡 只有 log |
| **傳輸** | RST、半幀、對端不讀 | recv/send | 收線（close）、reset parser、逾時 | ✅ |
| **服務** | 資源拿不到、start 失敗 | 服務管理器 | 該服務不啟動，其他照跑 | ✅ |
| **USB** | MSC 不支援（P4 rev1.3） | 開機 | 保留替代路徑（HTTP / NC4 file） | 🟡 HTTP 已做 |

---

## 2. 設定檔失敗

### 2.1 讀（開機）

```
讀 /data/config.json
  ├─ 不存在      → 用內建預設（第一支 firmware 也是這樣跑起來的）
  ├─ 不是合法 JSON → 用內建預設 + ESP_LOGE（不要停在 bootloader）
  ├─ 欄位缺       → 該欄位用預設
  └─ 值超範圍     → clamp + 警告（例：slots > 16 → 16）
```

**不做**：因為 config 壞掉就 `abort()`、進 boot loop、或把自己格式化。

### 2.2 寫（HTTP PUT，已實作 ✅）

```
PUT /config.json
  ├─ 大小 0 或 > 16 KB     → 400
  ├─ 不是合法 JSON          → 400（**不寫**）
  ├─ 合法                   → 寫 .tmp → 舊檔改 .bak → .tmp 換上 → {"ok":1}
  └─ 寫入失敗               → 500，並把 .bak 換回來
```

實測：

```
$ curl -X PUT --data-binary 'not json' .../config.json
400
$ curl -X PUT --data-binary @config.json .../config.json
{"ok":1,"next_boot":1}
$ curl .../config.json
{ "version": 1, "System": ... }
```

> 注意：`next_boot` 是因為**目前設定只在開機讀**；之後接上 config loader 才會即時生效。

### 2.3 `boot_strict`（規劃）

| 值 | 行為 |
|---|---|
| 0（預設） | config 壞 → 用預設，照常開機 |
| 1 | config 壞 → 停在 RECOVERY（只開 USB/HTTP，不跑應用） |

---

## 3. 網路失敗

### 3.1 開機（已實作 ✅）

- PHY 不是 **100 Mbps full** → 讀寫 BMCR 重啟 autoneg，等 3 秒再驗。
- 仍不是 100M full → `net_eth_start()` 回錯誤 → **不宣告 tcp.rx、不建 TCP 通道**。
  （`s_eth_ok` 控制宣告，見 config-layers.md §3）

### 3.2 執行期（⬜ 待做）

| 事件 | 現在 | 應該 |
|---|---|---|
| link down | 只印 log | 關閉 NC4 socket、停止 accept、標記 `net.down` |
| link up | 只印 log | 重建 listener、reset parser、恢復 accept |
| 對端 RST | 收線（✅） | 已足夠 |
| 對端不讀（send 卡住） | `SO_SNDTIMEO=1s` → 收線（✅） | 已足夠 |

> 執行期的 link down/up 需要一個小小的 state machine（`net_up / net_down`），
> 由 `eth_event_cb` 驅動；連線層只認「up 才可以 accept」。

---

## 4. USB：MSC 不是每型號都有

### 4.1 現況

| 板子 | USB MSC（隨身碟） | USB CDC（指令） | 檔案怎麼讀寫 | 指令怎麼下 |
|---|---|---|---|---|
| **S3** | ✅ | ✅ | 拔下來插電腦 / 或 HTTP | CDC / UART / 網路 |
| **P4 rev1.3** | ❌（TinyUSB 沒列舉成功） | ❌ | **HTTP `/config.json`** | TCP / UDP / WS |

### 4.2 這輪修掉的坑（重要）

P4 原本還是照 S3 的流程呼叫 `storage_hand_over_to_usb()` —— 把 FAT 從 APP 卸載、
掛給「不存在的 USB」。結果：**裝置自己讀不到 /data，電腦也看不到**，兩邊都拿不到檔案。

現在：

```c
#if CONFIG_IDF_TARGET_ESP32P4
    // P4 沒有 USB → /data 留在 APP，改用 http://<ip>:8080/config.json
#else
    storage_hand_over_to_usb();
#endif
```

### 4.3 替代路徑（依成本排序）

| 方法 | 能做什麼 | 狀態 |
|---|---|---|
| **HTTP `/config.json`** | 讀/寫設定（驗證 JSON） | ✅ 已上板 |
| HTTP 檔案列表 / 上傳 | 讀寫 assets（燈效、圖片） | ⬜ 同一個 httpd 再加 `GET/PUT /fs/<path>` |
| NC4 file service（0x21xx） | 大檔、斷點續傳、commit/rollback | ⬜ 架構已定（architecture-flow §3） |
| USB CDC（S3） | 直接送 NC4 指令 | ✅ S3 有；P4 無 |
| SD 卡 / 換板子 | 實體取得檔案 | — |

> 對 P4：**設定走 HTTP，指令走 TCP/WS/UDP**，功能上不缺；缺的是「大量檔案上傳」。

---

## 5. 開機失敗（服務層）

- 每個服務的資源**逐項**取得；拿不到 → 該服務不啟動，其他照跑（✅）。
- 服務 `start()` 回錯誤 → 標記 `started:0`，不影響其他人（✅）。
- `0x1101 keys=svc` 看得到誰啟動、誰的哪一項資源失敗（✅）。

---

## 6. 待辦（依優先序）

| # | 項目 | 為什麼 |
|---|---|---|
| 1 | `net_up/net_down` state machine | 拔線/插線後能自己恢復 |
| 2 | config loader（含 clamp 與預設） | 現在 config 還沒被讀，寫了只影響「未來」 |
| 3 | HTTP `/fs/`（列表/上傳） | 沒有 MSC 時仍能換 assets |
| 4 | NC4 file service | 大檔 + commit/rollback |
| 5 | `boot_strict` / RECOVERY 模式 | 壞設定時可救援 |


---

## 7. 容災階梯（最後一定可達的路徑）

| 層 | 路徑 | 依賴 | 什麼時候用 |
|---|---|---|---|
| **L0 ROM** | **USB-Serial-JTAG**（`/dev/cu.usbmodem…`）＋ esptool | 晶片內建 | 韌體壞、app 死、網路死 → 重燒 |
| **L0 ROM** | **UART0**（P4: GPIO37/38）＋ BOOT(GPIO0) | 晶片內建 | USB 也壞、或要接 USB-UART 時 |
| **L1 救援視窗** | UART0 開機前 1–2 秒收救援指令 | app 有寫 | 設定壞掉、想停用通道（⬜ 待做） |
| **L2 應用** | TCP / WS / UDP / HTTP | app + 網路 | 日常 |

**UART0 每塊 ESP 晶片都有** —— 它是 ROM 的 console 與下載 UART，開機第一行就從那裡出來
（本板 log：`GPIO 38 and 37 are used as console UART I/O pins`）。

### 7.1 坑：碰 DTR/RTS 會讓板子「重開進下載模式」

實測（2026-11-02，P4）：

| 動作 | 結果 |
|---|---|
| 用預設參數開 `/dev/cu.usbmodem…` 並寫 `hello` | 沒事（uptime 146 → 148 s） |
| 同一個埠，改成 `dtr=False, rts=False` 再開 | **板子直接掉線、進下載模式**，要 esptool reset 才回來 |

原因：ESP 的 auto-reset 把 **DTR/RTS** 接到 EN / GPIO0；USB-Serial-JTAG 也把 CDC 的
control line 映射到同一組 reset/boot 序列（esptool 就是靠它自動進下載模式）。
**所以「亂送資料會重開」不是資料的問題，是開埠時動了控制線。**

規則：

1. 任何 terminal 開這個埠**不要主動拉低 DTR/RTS**；不確定就先用預設值。
2. 要自動 reset 就用 esptool（它知道正確序列），不要自己 toggle。
3. App **不要**去搶 USB-Serial-JTAG 或 UART0 —— 那是 L0 救援路徑，搶走就沒有退路了。
4. 如果把 USB-Serial-JTAG 拿來當指令通道（規劃中），必須先確定「不碰 control line」也能收發。
