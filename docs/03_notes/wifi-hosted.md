# P4 的 Wi-Fi：外掛協處理器（ESP-Hosted）

> 2026-11-11。**性質：已上板驗證（P4 rev1.0 + C6，經 SDIO）—— 連上 AP、取得 IP。**
> 這一頁講「P4 沒有 radio 時，Wi-Fi 從哪裡來、型號怎麼換」。

---

## 1. 一句話

**P4 沒有 Wi-Fi；板上另有一顆 ESP32-C6／C5（或其他型號）做 radio，P4 經 ESP-Hosted 用它。**
應用層（`nc_wifi.c`、`network.json`）完全不用改 —— `esp_wifi_remote` 提供**同一個**
`esp_wifi.h`，所以 S3 的原生 Wi-Fi 與 P4 的遠端 Wi-Fi 是同一個 API。

```text
nc_wifi.c ── esp_wifi.h ──► esp_wifi（remote shim）──► esp_hosted ──SDIO──► ESP32-C6
   │                            ▲                                            │
   └── network.json（enable/ssid）┘                                      radio + TCP/IP 上半
```

- Wi-Fi 連線狀態、`esp_wifi_scan_*`、`esp_now_*` 全部經同一條路徑轉發。
- lwIP 仍然在 **P4** 這一邊（host），不是 C6；所以 socket／NC4 一行都不用改。

---

## 2. 型號不是只有一種

| 協處理器 | profile（`ports/_profiles/`） | sdkconfig 的關鍵一行 |
|---|---|---|
| ESP32-C6 | `wifi-hosted-c6` | `CONFIG_SLAVE_IDF_TARGET_ESP32C6=y` |
| ESP32-C5 | `wifi-hosted-c5` | `CONFIG_SLAVE_IDF_TARGET_ESP32C5=y` |
| 其他（官方支援的 slave target） | 複製一份改這一行 | `CONFIG_SLAVE_IDF_TARGET_<CHIP>=y` |

**換型號 = 換 board.json 的 profiles ＋ `tools/board.py sync` ＋ 重 build**，
`firmware/` 的程式碼不動。

```jsonc
// ports/P4/ESP32-P4-C6/board.json
"profiles": ["eth-rmii", "wifi-hosted-c6"],
"features": { "eth": "rmii-ip101", "wifi": "hosted-c6" }
```

---

## 3. 前提：slave firmware 要版本匹配

協處理器上要跑 **esp_hosted slave firmware**，而且 host／slave 版本要對得上
（本專案目前拉到 `esp_hosted 3.1.0`、`esp_wifi_remote 1.6.5`）。
版本差太遠時，開機 log 會停在 `esp_hosted` 的握手階段而不是 crash —— 先看 log 再懷疑硬件。

> 這塊 P4-C6 之前跑 MicroPython 的 `C6_WIFI` variant 時就已經有 slave firmware；
> Net-Core 只是換掉 host 那一邊。

---

## 4. 動了哪些地方

| 檔案 | 改動 |
|---|---|
| `firmware/main/idf_component.yml` | 加 `esp_wifi_remote`／`esp_hosted`，**用 `rules: target == esp32p4` 限定**，S3 不受影響 |
| `firmware/main/Kconfig.projbuild` | 新增 `NETCORE_WIFI_HOSTED`（只出現在 P4）；`NETCORE_WIFI` 的 `depends` 放寬為「有 radio **或** 有 hosted」 |
| `firmware/main/CMakeLists.txt` | P4 + hosted 時 `PRIV_REQUIRES` 加 `esp_wifi esp_wifi_remote` |
| `firmware/components/action/test_action/CMakeLists.txt` | `esp_event`／`esp_netif` 由「只在 S3 加」改成「有 `CONFIG_NETCORE_WIFI` 就加」——**這是原本就存在的隱性 transitive 依賴**，P4 一開 Wi-Fi 就爆 |
| `ports/_profiles/wifi-hosted-c6.defaults`／`-c5.defaults` | 能力碎片（來源：MicroPython `sdkconfig.p4_wifi_common`，去掉 BLE） |
| `ports/P4/ESP32-P4-C6/` | 新板：16 MB flash、32 MB HEX PSRAM、P4 rev1.0、C6 作 Wi-Fi |

### 為什麼要 `CONFIG_SLAVE_*`

那組是 esp_hosted 的「對端能力宣告」：host 靠它決定要開哪些功能（WAPI／CSI／MESH…）。
它跟**實際燒在 C6 上的韌體**是兩件事 —— 寫錯的症狀是「連得上但某些 API 回 not supported」。

---

## 5. 未做 / 已知限制

| 項目 | 現況 |
|---|---|
| BLE | 未接（MicroPython 的 profile 有 NimBLE，這裡刻意拿掉：Net-Core 還沒有 BT 層） |
| slave firmware 燒錄 | 未接自動化；C6 上現有的 firmware 是之前 MicroPython 留下的 |
| SDIO 腳位覆蓋 | 目前用 `CONFIG_ESP_HOSTED_P4_DEV_BOARD_FUNC_BOARD`（官方 P4 開發板腳位）。板子接法不同時要覆蓋 `CONFIG_ESP_HOSTED_SDIO_*`，**另開一個 profile**，不要改現有兩個 |
| 記憶體 | 遠端 Wi-Fi 的緩衝是 `WIFI_RMT_*`（`ESP_WIFI_*` 是衍生值，寫了沒用） |
| `CONFIG_PM_SLEEP_CLK_ICG_ENABLE` | P4 rev0/1 + 大靜態 DRAM（MicroPython 那種）會 boot abort；Net-Core 目前沒踩到，但若加了 hosted 後出現 `sleep_clock_icg_init` 失敗，把它設 `n` |
