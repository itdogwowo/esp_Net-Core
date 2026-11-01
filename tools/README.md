# tools/ — 主機端工具

這些東西**不會**編進 firmware。它們在你的 Mac 上跑，透過序列埠或網路對裝置說話。

分類規則：**`tools/` 頂層 = 你平常會直接打的命令；子目錄 = 階段性驗證。**

```text
tools/
├─ ncbuild/                     ① 建置工具（make.py 的實作）
├─ board.py                     ① 板子清單 / 設定檢查 / 新增板子
├─ board.sh                     ① 薄殼 → make.py（相容用）
├─ flash.sh                     ① 逃生門燒錄
├─ nc4.py  ota.py selftest.py nc4_test.py   ② 對設備說話
├─ bench/                       ③ 量測（要板子）
├─ hosttest/                    ④ 主機端 C 測試（不需要板子）
└─ poc/                         ⑤ 原型驗證
```

---

## ① 建置工具（`make.py`）

入口在 repo 根：`python3 make.py`。實作在 `tools/ncbuild/`：

```text
make.py                 薄殼（10 行）
tools/ncbuild/
├─ cli.py               命令與參數
├─ env.py               路徑、IDF 解析、子行程（輸出同時寫日誌）
├─ builder.py           build / flash / monitor / clean / config
├─ doctor.py            環境健檢
└─ artifacts.py         時間戳產物 + sha256 側錄 + prune
```

| 命令 | 需要板子 | 做什麼 |
|---|---|---|
| `make.py p4` = `make.py build p4` | — | 建置；自動處理 target 切換 |
| `make.py build s3 --minimal` | — | 最小映像（只印心跳），新板 bring-up 用 |
| `make.py build p4 --fresh` | — | 先刪 `sdkconfig` 再建（改了 defaults 才需要） |
| `make.py build p4 --jobs 4` | — | 限制平行編譯 |
| `make.py doctor` | — | 環境健檢：IDF / idf.py / esptool / pyserial / 板子設定 / 磁碟 / git |
| `make.py flash p4 [--full\|--erase]` | ✅ | 建置 + 燒錄（預設 `app-flash`，不動碟上的使用者資料） |
| `make.py monitor p4` | ✅ | 序列埠監看 |
| `make.py outputs [--prune N]` | — | 列出歷史產物 / 每塊板子只留 N 顆 |
| `make.py config p4` | — | 印出這塊板子的 board.json + 合併後的有效 Kconfig |
| `make.py clean [--all]` | — | 清 `build/` `sdkconfig`；（`--all` 連 `managed_components`） |
| `make.py boards` / `list` / `ports` / `env` | — | 板子表 / 插著的板子 / 序列埠 / IDF 路徑 |

### 產物（`artifacts/`）

每次建置成功會複製一顆帶時間戳的 image，並附一份 `.json` 側錄：

```text
artifacts/
├─ ESP32-P4-ETH_20261102_022530.bin
├─ ESP32-P4-ETH_20261102_022530.json    ← board / chip / git / idf / sha256 / 尺寸
└─ logs/ESP32-P4-ETH_20261102_022530.log
```

出貨或 OTA 回滾測試時直接拿這個檔案，不用回頭挖 `firmware/build/`。
`--no-archive` 可關掉；`outputs --prune 3` 只留每塊板子最近 3 顆。

### 設定檔

`make_config.json`（root）：`default_board`、`build.jobs`、`build.keep`、
`idf.per_chip`（讓 P4 用不同的 IDF）、`serial.port`。命令列參數優先。

### 跟舊 `board.sh` 的關係

`tools/board.sh` 現在只是 `exec python3 make.py "$@"` 的相容殼——**一個實作，不會兩邊漂移**。
`--console jtag|otg` 拿掉了：console 屬於板子，寫在 `board.json` 的 `console` 欄位。

`flash.sh` 仍然保留，用於「TinyUSB 佔住 USB PHY、`idf.py` 進不了 download mode」的情況：
它會等你按 BOOT、燒完再 watchdog reset 踢回 app。

### `board.py` — 板子設定的真相

板子清單不是寫在腳本裡，而是 `ports/<chip>/<board>/board.json`：

```bash
python3 tools/board.py list      # 所有板子 × flash/PSRAM/ETH/console
python3 tools/board.py check     # 驗 board.json ↔ sdkconfig；擋下「把 build-time 設定寫進 config.json」
python3 tools/board.py sync      # 改了 ports/_profiles/ 之後，把能力碎片寫進各板
python3 tools/board.py new --chip S3 --board ESP32-S3-Matrix --flash 16 --psram 8 --ws
```

新增板子**不需要改 `board.sh` 或 CMake**。`board.sh` 會自己問 `board.py` 這塊板子的
目錄、idf target 與 IDF 路徑。細節見 [ports/README.md](../ports/README.md)。

## ② 對設備說話

| 工具 | 需要板子 | 做什麼 |
|---|---|---|
| `nc4.py --port … reboot\|raw\|listen` | ✅ | 送單一 NC4 指令 / 監看輸出 |
| `ota.py --port … <image>` | ✅ | NC4 OTA 推 firmware，**不需要按鍵、不需要 download mode** |
| `selftest.py --port …` | ✅ | 一鍵自測：STATUS / 解碼器 9 case / 效能 / 突發 / OTA（會匯入同層的 `ota.py`） |
| `nc4_test.py --port …` | ✅ | 解碼器一致性測試：正常幀 / 黏包 / 拆包 / 壞 CRC / 假 SOF / 錯 addr / 超大 LEN / 錯版本 |

firmware image 在 `firmware/build/esp_net_core.bin`（先 build 才有）。

## ③ `bench/` — 量測（要板子，多數要 P4 乙太網）

| 工具 | 量什麼 |
|---|---|
| `bench_all.py` | 一次跑完 A–F 六組：純 TCP / NC4 下載 / NC4 上傳 / 純 CPU / 雙核 hub / 匯流排統計 |
| `net_bench.py` | 純 TCP 吞吐（port 5000/5001/5002），驗網卡上限 |
| `nc4_tcp.py` | NC4 over TCP 吞吐與延遲（port 6000） |
| `ws_nc4_test.py` | NC4 over WS 全套：RTT / 吞吐 / 碎片 / 重連 / 並存 |
| `udp_nc4_test.py` | NC4 over UDP：RTT / 吞吐 / 掉包 |
| `nc4_stats.py` | 0xF008 匯流排統計（`copied` 應為 0）＋ 碎片/垃圾重同步邊界 |
| `nc4_tx_bench.py` | 0xF007 上傳（打包＋送出），同核 / 分核 |
| `hub_bench.py` | 0xF003 雙核 hub 交換量測 |

P4 預設靜態 IP `10.10.1.50/16`，量測前先確認連得到。

## ④ `hosttest/` — 主機端 C 測試（不需要板子）

用 `cc` 直接把同一個 `.c` 編進來跑，驗證純邏輯，不用燒板子：

```bash
tools/hosttest/hub_test/run.sh     # nc_hub.c：格子交換 / 自動歸還
tools/hosttest/plan_test/run.sh    # nc_plan.c：宣告 / commit / 重名
```

兩支都會印 `ALL PASS`。改動 `components/sys/mem/` 之後先跑這個。

## ⑤ `poc/` — 原型驗證

| 工具 | 需要板子 | 做什麼 |
|---|---|---|
| `poc/fx_poc/host.mjs` | ❌ | 效果模組 ABI 的 host 參考實作（node，零依賴）：跑模組、中途換模組、餵壞模組 |

```bash
node tools/poc/fx_poc/host.mjs tools/poc/fx_poc/fx.wasm --frames 120
```

---

## 還沒有的工具

文件提到但**目前不存在**，屬於計劃產物，不要當成壞掉的路徑：

- `tools/fx.py` — 效果模組打包 / 上傳工具（[fx-loader.md](../docs/03_notes/fx-loader.md) M1、[effect-language.md](../docs/03_notes/effect-language.md) P1 的產物）
- `tools/fixtures/` — 測試樣本（[component-layout.md](../docs/03_notes/component-layout.md) §8 規劃）
