# 專案現況總結（單頁）

> **用途**：所有「已經驗證的事實、已定案的設計、還沒做的事」集中在這一頁；
> 細節都連到對應文件。**最後更新**：2026-11-02。

---

## 1. 這是什麼

ESP32-P4（另有 S3 port）上的 **NC4 通訊核心**：多條傳輸通道 → 解碼 → 指令執行，
外加分核（RX core1 / decode core0）、零複製、可調的緩衝管理。

| 面向 | 狀態 |
|---|---|
| 晶片 | ESP32-P4 rev1.3（ECO2）、360 MHz、32 MB PSRAM、IDF 6.0.3 |
| 網路 | 100 Mbps full duplex（RMII + IP101），靜態 IP `10.10.1.50/16` |
| 通道 | TCP :6000、UDP :6001、WS :8080/nc4、HTTP :8080（設定） |
| 解碼 | 分核、零複製（一格一整幀）、task notification |
| 架構 | System（硬體→driver→通道）＋ Service（生命週期）已分層 |

---

## 2. 已驗證的數字（2026-11-02，同一台板子）

### 通道吞吐（16 MB/筆，MB/s）

| 通道 | 下載 1K / 4K / 8K | 上傳 1K / 4K / 8K | RTT |
|---|---|---|---|
| **TCP**（分核＋零複製） | 5.65 / 7.13 / **7.40** | 6.05 / 7.43 / **7.42** | **3.56 ms** |
| **WS**（多幀 batch） | 4.30 / 5.72 / 5.26 | 4.99 / — / 6.98 | 6.87 ms |
| **UDP**（≤ MTU） | 1.93（掉 16%）/ ✗ / ✗ | **8.51**（掉 0%）/ ✗ / ✗ | **3.73 ms** |

### 上限與參考

| 項目 | 數值 | 卡在 |
|---|---|---|
| **NC4 單向上限** | **7.5–8.5 MB/s** | TCP 視窗 65,535 ÷ RTT；UDP 卡 MTU |
| TCP 全雙工 | 11.9–12.4 MB/s | 100 Mbps PHY（97–99%） |
| TCP 雙 stream | 9.4–9.9 MB/s | 同上 |
| 純解析（CPU） | 30.7 MB/s（267 µs/8 KB） | CRC32 |
| 純打包（CPU） | 28.8 MB/s（285 µs/8 KB） | CRC32 |
| hub 交換 | 326k–517k /s、torn=0 | 系統負載 |

### 零複製

| 模式 | `copied` | 每幀解析 | 吞吐 |
|---|---|---|---|
| PUSH-slot（兩次複製） | 33.5 MB / 16 MB | 495 µs | 7.18–7.87 |
| **PUSH-frame（零複製）** | **0 B** | **406–418 µs（−18%）** | 7.41–7.53 |

- soak：10 輪 × 16.4 MB → 20,020 幀全到、`resync=0`。
- 真正讓吞吐提升的是「**不要 10 ms 輪詢**」（task notification）：1 KB 幀 0.78 → 5.65 MB/s，
  RTT 10.1 → 3.6 ms。

---

## 3. 已定案的架構

| 主題 | 一句話 | 文件 |
|---|---|---|
| **System vs Service** | 硬體/driver/NC4 通道是 System（固定順序）；效果/UI 才是 Service（有生命週期） | [config-layers.md](../03_notes/config-layers.md) |
| **開機順序** | 硬體 → driver → **宣告（看實際硬體）** → carve → 通道 → 服務 → 任務 | 同上 |
| **服務生命週期** | `declare() → start(res) →（task loop）→ stop()`；服務只宣告，管理器負責取 | [writing-a-service.md](../03_notes/writing-a-service.md) |
| **資源管理器** | `nc_plan`：declare → commit → get（目前沒有 release） | [buffer-manager.md](../03_notes/buffer-manager.md) |
| **收/解路徑** | 傳輸總線（ID 註冊表）→ 解碼引擎（可換核/可多實例）→ 執行 | [rx-architecture.md](../03_notes/rx-architecture.md) |
| **配置分層** | System / Hardware / Driver / Network / Tasks / Effects / UI | [config-layers.md](../03_notes/config-layers.md) |

**現況**：`main.c` 已有 `s_sys_parts[]`（System 固定順序）＋ `nc_service`（應用服務）；
config.json 還沒接（目前用程式預設 + NVS）；slots 還沒走 plan。

---

## 4. 失敗處理（已實作 / 待做）

| 類別 | 政策 | 狀態 |
|---|---|---|
| config 壞掉 | 用預設、驗證後才寫、壞的不覆蓋 | 🟡 讀寫已通（HTTP），載入未做 |
| PHY 協商失敗 | 讀寫 BMCR 重啟 autoneg，仍失敗就不啟用通道 | ✅ 實測救回 |
| 拔線 / link down | 關連線、停 accept，link up 重建 | 🟡 只有 log |
| 傳輸錯誤 | 錯誤一律收線、reset parser、閒著 ≠ 壞線 | ✅ |
| 服務失敗 | 逐項失敗，該服務不啟動、其他照跑 | ✅ |
| USB 不支援 | 走 HTTP / USB-JTAG 替代 | 🟡 HTTP 已做 |

詳細：[failure-policy.md](../03_notes/failure-policy.md)。

---

## 5. 硬體事實（踩過才知道）

| 事實 | 影響 |
|---|---|
| P4 這塊板的 USB-C **只接 USB-Serial-JTAG**，沒有接 USB-OTG | TinyUSB（CDC/MSC）永遠列舉不到；但**燒錄/reset 完全正常** |
| 啟用 TinyUSB 後 `tinyusb_console_init()` 會把 console 搬走 | 連 log 都看不到 → 已關閉 |
| **DTR/RTS → GPIO0/EN**（USB 橋或 USB-JTAG 都一樣） | 開埠時動到控制線 → reset + 下載模式（實測：`dtr=False,rts=False` 直接進下載） |
| 下載模式只看「reset 時 GPIO0 的電平」 | **送資料本身永遠不會觸發下載模式** |
| PHY 沒有 reset GPIO | MCU reset 不會重置 PHY → 需要軟體重啟 autoneg |
| UART 數量 | **P4 = 6 條、S3 = 3 條**；console 佔 UART0，其餘可給應用 |
| P4 console 腳位 | GPIO37/38（不是 S3 的 43/44） |
| ESP↔ESP 不能用 USB 對接 | 兩個都是 device，沒有 host；VBUS 還會互灌 → 請走 UART1/2 |

USB 的完整分析：[failure-policy.md §4](../03_notes/failure-policy.md)。

---

## 6. 工具

| 工具 | 用途 |
|---|---|
| `tools/board.sh` | 建置/燒錄（會自動切 target） |
| `tools/bench/bench_all.py` | TCP 全表：純 TCP / NC4 上下傳 / CPU / hub |
| `tools/bench/nc4_tcp.py` / `nc4_tx_bench.py` | TCP 下載 / 上傳 |
| `tools/bench/ws_nc4_test.py` | WS：RTT / 吞吐 / 碎片 / 重連 / 並存（10 項） |
| `tools/bench/udp_nc4_test.py` | UDP：RTT / 吞吐 / 掉包 |
| `tools/bench/nc4_stats.py` | 零複製驗證（`copied` 應為 0）+ 邊界測試 |
| `tools/bench/hub_bench.py` | `0xF003` 雙核 hub 交換 |
| `curl http://10.10.1.50:8080/config.json` | 讀寫設定（GET/PUT） |

---

## 7. 還沒做（依優先序）

| # | 項目 | 為什麼 |
|---|---|---|
| 1 | **link down/up state machine** | 拔線/插線後能自己恢復（目前只有 log） |
| 2 | **config loader**（含 clamp/預設） | 現在寫入只影響下次開機 |
| 3 | **目錄拆分**（tcp_nc4 / eth 抽成 component） | `main.c` 還是很大 |
| 4 | **USB-Serial-JTAG 指令通道** | P4 就能用 USB 送指令（不用 TinyUSB） |
| 5 | UART0 救援視窗（開機 1.5 s） | 所有路徑都壞時的最後一條 |
| 6 | HTTP `/fs/`（列表/上傳）+ NC4 file service | 沒有 MSC 時換 assets |
| 7 | slots 走 plan、`nc_task_stop` | 生命週期完整 |
| 8 | S3 實機重燒 + 長時 soak | 迴歸 |

---

## 8. 待測場景（等使用者提供）

以下需要**實體接線/操作**才能確認，程式面已就緒或已避開：

| 場景 | 想確認什麼 | 需要的條件 |
|---|---|---|
| 拔線 / 插線 | link down→up 後通道是否自動恢復 | 可插拔的網路線 |
| ESP↔ESP（UART1/2） | 下載模式是否還會發生 | 兩塊板、只共地、不接 USB |
| 同一條 UART0 收指令 | console 與資料共存是否穩定 | USB 拔掉、只剩 UART0 RX |
| 壞 config | 開機是否仍能起來 | 手動改壞 `/data/config.json` |
| 電源不穩 | 是否 brownout | 可調電源 |

---

## 9. 文件索引

| 類別 | 文件 |
|---|---|
| 協議 | [commands.md](../01_protocol/commands.md)、[transports.md](../01_protocol/transports.md) |
| 教學 | [build_and_flash.md](../02_guides/build_and_flash.md)、[testing.md](../02_guides/testing.md)（§2.13–2.16 是量測總表） |
| 筆記 | [config-layers.md](../03_notes/config-layers.md)、[writing-a-service.md](../03_notes/writing-a-service.md)、[rx-architecture.md](../03_notes/rx-architecture.md)、[failure-policy.md](../03_notes/failure-policy.md)、[p4-bringup.md](../03_notes/p4-bringup.md)、[buffer-manager.md](../03_notes/buffer-manager.md)、[memory.md](../03_notes/memory.md)、[component-layout.md](../03_notes/component-layout.md)、[architecture-flow.md](../03_notes/architecture-flow.md)、[rewrite-plan.md](roadmap.md) |
| 研究 | [effect-language.md](../03_notes/effect-language.md)、[extension-architecture.md](../03_notes/extension-architecture.md)、[fx-loader.md](../03_notes/fx-loader.md) |
