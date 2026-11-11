# IO 與連接埠：P4 / S3 每條線係咩、用嚟做咩

> 2026-11-11。**這一頁是查表用的**：燒錄、看 log、連 NC4，先來這裡對一次，
> 不要再靠記憶（之前就係因為漏記，重複問「S3 點燒」「P4 有幾個 USB」）。

---

## 0. 兩個板一覽

| 板子 | 晶片 | console | 燒錄（ROM） | NC4 資料通道 |
|---|---|---|---|---|
| **S3-N16R8** | ESP32-S3 | USB-Serial-JTAG（次要） | **UART0**（預設）／USB-Serial-JTAG／`DFU` 字串 | USB CDC（TinyUSB，`303A:4003`）、Wi-Fi |
| **P4-C6** | ESP32-P4 rev1.0 | **UART0（主）** ＋ USB-JTAG（次） | **UART0**（預設）／USB-Serial-JTAG | Wi-Fi（外掛 C6，hosted）、ETH（未通）、UART1 |

---

## 1. P4 的 USB：**三條獨立 PHY**（不是共用）

| PHY | D+ | D- | 預設接什麼 |
|---|---|---|---|
| **FS_PHY1** | **GPIO25** | **GPIO24** | **USB-Serial-JTAG**（燒錄／console） |
| **FS_PHY2** | **GPIO27** | **GPIO26** | **OTG_FS**（TinyUSB） |
| **HS_PHY** | pin 50 | pin 49 | OTG_HS（專用腳，不能當 GPIO） |

來源：ESP-IoT-Solution [USB PHY/Transceiver](https://docs.espressif.com/projects/esp-iot-solution/en/latest/usb/usb_overview/usb_phy.html)。
預設連線可用 eFuse `USB_PHY_SEL` 改，但**燒了就不可逆**。

**重要**：三條 PHY 是**獨立**的 —— 所以 TinyUSB 在 FS_PHY2 上跑起來，
**不會**影響 FS_PHY1 的 USB-Serial-JTAG console（這同 S3 的「共用一條 PHY」完全不同）。

> 推論：Firebird 那一個 USB-C 枚舉成 `303A:1001`，代表它接的是 **FS_PHY1（24/25）**。
> 要做 TinyUSB，就要把 D+/D- 接到 **GPIO27（D+）／GPIO26（D-）**，或者用 HS（50/49）。
> 這也是「插住燒錄那條線，但 OTG 永遠不出現」的原因。


## 2. 燒錄：四條路，優先次序

| 路 | 何時用 | 指令 |
|---|---|---|
| **UART0**（預設） | USB 死咗、想最稳；P4 的 console 本身就在這 | `esptool -p COMx write-flash …`（要 USB-UART 線） |
| **USB-Serial-JTAG** | P4 日常（`COM21`，每次 reset 換號） | `esptool --chip esp32p4 -p COM21 write-flash …` |
| **CDC `DFU` 字串** | S3：USB 只有 TinyUSB 時，令它重枚舉成 ROM | 對 CDC 埠送 `DFU`，或按住 BOOT 5 秒 |
| **NC4 OTA（0x22xx）** | 兩塊板都支援；**不需要任何線／按鍵** | `tools/ota.py --port COM8 <bin>`（S3）／HTTP `POST /ota`（有網時） |

> S3 實測：`WRITE ok（3.2 s，165.2 KB/s）→ END ok → APPLY` 約 1 分鐘完成整個更新。

## 3. UART

| 板子 | UART | 腳位 | 用途 |
|---|---|---|---|
| P4 | UART0（官方預設） | GPIO37 (TX) / 38 (RX)，115200 | **console ＋ ROM download 預設** |
| S3 | UART0（官方預設） | 由 `wiring.json` 決定（目前 `null` = 未啟用） | 預設燒錄／log |

> 只記官方預設值。測試用的暫設（例如臨時 921600 的 link）不寫進來 —— 那些是每次 build 都可以不同。

> **選 JTAG 定 OTG？** 建議 **OTG**（TinyUSB）：FS PHY（GPIO26/27）上二者只能二選一，
> 但 ROM download 是**開機初期**的事，應用程式跑起來之後才切去 OTG，兩者不衝突。
>
> 而且**「冇次 console」這個顧慮不成立**：S3 就是同一條路 —— TinyUSB CDC 起來之後，
> `nclog` 把 log 用 RS(0x1E) 文字幀寫入同一條 CDC，同 NC4 二進位幀共用（`cdc_tx_text`）。
> 所以 P4 切去 OTG 之後，得到的是一條「NC4 數據 ＋ console」的 CDC，**同 S3 一模一樣**。
>
> 燒錄方面：本來就有 **NC4 OTA**（`tools/ota.py`），ROM 燒錄只是逃生門；真要 ROM 就手動 BOOT+EN。

## 4. 一眼看懂的接線決定

```
要燒 P4      → UART0（最稳）或 USB-JTAG（COM21）
要看 P4 log  → UART0（主）／USB-JTAG（次，兩邊都有）
要 NC4 通 P4 → Wi-Fi（hosted C6）／ETH（PHY 未配對）／UART1
要燒 S3      → NC4 OTA（首選）／DFU 字串 → esptool／UART0
要 NC4 通 S3 → USB CDC（COM8）／Wi-Fi
```

## 5. 已知未通

| 項目 | 症狀 | 缺什麼 |
|---|---|---|
| P4 ETH | `emac_esp32_init: reset timeout` | Firebird 的 PHY 型號／MDC/MDIO/RST 腳位 |
| P4 TinyUSB | `ph_usb()` 對 P4 硬編碼 `NOT_SUPPORTED` | 拆 guard 再 build（FS PHY 26/27） |
| S3 UART | `wiring.json` 的 tx/rx 是 `null` | 選腳位（避開 SDIO 已 claim 的） |

---

## 9. 結論（2026-11-11，Firebird ESP32-P4 v1.0）

| 事實 | 證據 |
|---|---|
| 板上唯一 USB-C 接在 **FS_PHY1（GPIO24/25）**，即 USB-Serial-JTAG | 一直枚舉成 303A:1001；燒 OTG build 之後仍然係它 |
| **OTG（TinyUSB）沒有接出來** | 燒 TINYUSB_PORT_FULL_SPEED_0 版本之後，FS_PHY2（26/27）同 HS（49/50）都冇新裝置 |
| 三條 PHY 是**獨立**的 | 所以 TinyUSB 唔會弄死 JTAG console —— 前提係嗰對線有接 |

**這塊板做不到「USB 同 S3 一樣」，缺的是硬件（一對 D+/D-），唔係軟件。**
軟件已就緒：NC_HAS_USB 能力巨集 ＋ 	usb_cfg.port = TINYUSB_PORT_FULL_SPEED_0。
只要有人接出 **GPIO27 (D+) / GPIO26 (D-) / GND**（或 HS 的 50/49），同一個 build 就會動。

> 提示：唔好再花時間查「軟件點解唔枚舉」——先量嗰兩隻腳有冇接到連接器。

---

## 10. 更正（2026-11-11）：這塊板係 **FireBeetle 2 ESP32-P4（DFR1172）**，官方寫 **Type-C OTG 2.0**

- 板名之前寫錯成「Firebird」；正確係 DFRobot **FireBeetle 2 ESP32-P4（ESP32-P4R32）**。
- 官方 wiki tech spec：**Type-C OTG 2.0**、on-board ESP32-C6（Wi-Fi 6）、MIPI DSI／CSI、MIC、TF。
- 即係話 §9 的「OTG 冇接出來」結論**同官方說法相反**，要重新確認：
  1. 板上可能有**兩個** Type-C（一個 USB-Serial-JTAG／燒錄、一個 OTG 2.0）
  2. 我們一直插的那個枚舉成 303A:1001（JTAG），所以 OTG 應該是**另一條線／另一個孔**
- 下一步：確認插的是哪個孔；若只有一個孔，就要查 schematic（DFR1172_..._schematics_V1.0.pdf，向量圖冇文字層，要人眼看）。

---

## 11. 進度（2026-11-11 晚）：USB OTG 已枚舉，但仍有 crash #2

| 項目 | 結果 |
|---|---|
| 板上兩個孔 | USB CDC = FS_PHY1（GPIO24/25，JTAG）；USB OTG 2.0 = **HS_PHY（pin 50/49）** |
| 選對 PHY | 	usb_cfg.port = TINYUSB_PORT_HIGH_SPEED_0 → **303A:4003 出現**（COM13） |
| crash #1 | 
c_log_start(lc, cdc_tx_text) 一呼叫即 Load access fault → 已在 P4 停用 |
| crash #2 | **未定位**：過了 storage 之後仍 panic（每次開機一次） |
| 現狀 | NC_HAS_USB=0 已燒回，板子穩定、log 正常 |

### 下一手要做的

1. 在 oot[11] 之後、以及 cdc_rx_task / TinyUSB task 入口加 checkpoint，抓 crash #2
2. 最可疑：MSC（storage 回調）或 CDC 的 RX/TX task —— 兩者原本只為 S3 寫
3. 穩定了才做 log 管道（
c_log_start 要修，不是停用）與測試指令表

> 燒錄不會斷：USB CDC 孔的 JTAG（COM21）同 OTG 孔是**兩條獨立 PHY**，所以 OTG 出事仍然救得返。


---

## 12. 已解決（2026-11-11 晚）：crash #2 的根因就是「MSC 未安裝」

**一行改動**：`main.c` 的 `usb_install()` 裡，MSC driver 的條件由
`#ifdef CONFIG_IDF_TARGET_ESP32S3` 改成 `#if NC_HAS_USB`。

為什麼會出事：`NC_HAS_USB` 打開之後 P4 一樣會行整個 USB 服務組，
但那一行寫死了 S3 → **P4 從來冇人呼叫 `nc_store_usb_driver()`**。
於是 `s_driver_installed` 永遠是 0，`switch_to()`（nc_store.c:477）直接
`return ESP_ERR_NOT_SUPPORTED`，storage 交唔到 host；
主機端仍然按 composite descriptor 去探測 Mass Storage → 每次開機 crash 一次。

| 觀察點 | 改之前 | 改之後（同一塊板、同一個 build 目錄） |
|---|---|---|
| log | 冇 MSC 行 | `store: MSC driver 已安裝（async_io → core 0）` |
| `storage 交 host` | `ESP_ERR_NOT_SUPPORTED` | `USB-DBG: storage 交 host 之後 ESP_OK`／`碟交給 host: ESP_OK` |
| 重新列舉 | — | `store: USB 已重新列舉` |
| 主機 | 冇碟 | **`D:` NC-2EC0（7 MB FAT）**；`VID_303A&PID_4003&MI_02` → `Service=USBSTOR` |
| 碟內容 | — | `config/{hardware,network,system}.json`、`pixel/effects/` 都讀得到 |
| panic | 每次開機 1 次 | **0**（兩次開機、連續約 70 秒心跳，heap 349,095 → 355,755 穩定） |

> 結論：crash #2 **不是** TinyUSB/MSC 內部 bug，而是「descriptor 說有、driver 沒裝」
> 的狀態不一致。P4 的 MSC 路徑（async_io task、交出 FAT、重新列舉）全部正常。

### 還未做的

1. 小時級 soak ＋ 主機端大量寫入（S3 上做過的 6.3 MB 壓力測試，P4 未做）
2. `usb_reconnect()` 的 800 ms 視窗會不會影響正在連的 NC4 client

---

## 13. 更正（2026-11-11 晚）：§11 的「log 管道一呼叫即 panic」是誤判

§11 寫「`nc_log_start(lc, cdc_tx_text)` 一呼叫即 Load access fault → 已在 P4 停用」，
**不成立**。MSC 修好之後重測：

- `nc_log_start()` 回 **ESP_OK**，管道在 CDC 上正常輸出（`\x1e` 前綴、`0 `/`1 ` 核號）。
- 當時看起來像死機的原因：`hook_install()` 一裝，esp_log 就改走環＋CDC，
  **JTAG console 立即變靜**；而主機又冇開 COM13（`cdc_tx_text` 見 DTR 未拉高就回
  OFFLINE，故意不消費 ring）→ 兩邊都冇輸出。看起來就是「呼叫完就死」。
- 所以 §11 的 crash #1 應該同 §12 是同一個根因：USB 服務組（MSC）沒裝好。

**補做**：P4 的 console（USB-Serial-JTAG, GPIO24/25）同 OTG（HS PHY, pin 50/49）是
**兩條獨立 PHY**，所以 `log_hook` 在 P4 會 tee 一份去 `s_prev`（`#if CONFIG_IDF_TARGET_ESP32P4`）；
S3 共用一條，裝 hook 之後 JTAG 已經不在，tee 冇意義。這樣管道開著時 console 仍有全部 log，
main.c 那個「逃生門」不會被自己的 log 管道拆掉。

驗證：COM21（console）同 COM13（CDC 管道）同時收到同一批行，`log_start 之後 ESP_OK`。
---

## 14. 三層 config：哪些編譯期、哪些執行期（2026-11-11）

USB 的差異不再寫在 `.c` 裡。主程式**冇任何 `CONFIG_IDF_TARGET_*`**，分三層：

| 層 | 位置 | 決定什麼 | 換晶片／換板要改？ |
|---|---|---|---|
| ① 能力 | `Kconfig.projbuild` `NETCORE_USB_OTG` | 這段程式碼**存不存在**（tinyusb 約 21.7 KB） | 不用，default 已列 S3/S2/P4 |
| ② 板子 | `NETCORE_USB_OTG_HS`、`NETCORE_USB_CONSOLE_TEE` | 用邊條 PHY、log 要不要 tee console | 換板要（目前放 `ports/<board>/sdkconfig.defaults`） |
| ③ 執行期 | `system.json` 的 `usb.{msc,cdc_log}` | 開唔開、交唔交碟 | 不用，改完重開機即生效 |

**陷阱**：`SOC_USB_OTG_SUPPORTED` 是自動產生的 **C 巨集**，不是 Kconfig 符號。
寫 `default y if SOC_USB_OTG_SUPPORTED` 會**靜靜當成 n**（符號 undefined），
所以 Kconfig 裡只能用 `IDF_TARGET_*` 列目標。

驗證（2026-11-11，P4）：

```
I (4090) netcore: usb.cdc_log=0 → log 只走 console     ← system.json 改一個數字
I (4185) netcore: 碟交給 host: ESP_OK                  ← msc 仍為 1
```

`cfg: system: ... unk=0` 也證明 `usb` 這個鍵已被辨識（不會被當成不認識的欄位）。

還沒做：搬去 `ports/_profiles/usb-otg-hs.defaults`（由 board.json 宣告），
以及 `0x1103 STATUS_SET` 直接改 `usb.*` 並即時套用（目前要重開機）。