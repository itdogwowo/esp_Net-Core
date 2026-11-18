# tools/ — 主機端工具

這些東西**不會**編進 firmware。它們在你的 Mac 上跑，透過序列埠或網路對裝置說話。

分類規則：**`tools/` 頂層 = 你平常會直接打的命令；子目錄 = 階段性驗證。**

## 快速入口

- **網頁工作臺（ESP WebMaster）** 唔喺 `tools/`，全部收喺 repo 根嘅 **`web_tools/`**：
  - Windows：雙擊 `web_tools\WebMaster.bat`（或 `.\web_tools\web.ps1`）
  - macOS：雙擊 `web_tools/WebMaster.command`（或 `./web_tools/web.sh`）
  - Linux：`./web_tools/web.sh`
  - → http://127.0.0.1:8010/ ；文件：[web_tools/README.md](../web_tools/README.md)
- 建置／燒錄／看板：`python3 make.py`（見 `make.py --help`）
- 對設備說話：`tools/nc4.py --port …`
- 量測：`tools/bench/`；主機端測試：`tools/hosttest/`


```text
tools/
├─ ncbuild/                     ① 建置工具（make.py 的實作）
├─ ncportgen.py                 ① 建置期程式碼產生（ports/ → nc_port.c/h）
├─ sync_hardware_presets.py     ① 兩層 preset 同步 / 漂移檢查
├─ *_diagram.py                 ① 文件用圖（boot / buffer / core …；改了要重畫 SVG）
├─ board.py                     ① 板子清單 / 設定檢查 / 新增板子
├─ board.sh                     ① 薄殼 → make.py（相容用）
├─ flash.sh                     ① 逃生門燒錄
├─ nc4.py  ota.py selftest.py nc4_test.py nc4_file.py buf_test.py matrix.py ncm.py   ② 對設備說話 / 打包 `.nct` / `.nce`
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
| `make.py build ESP32-P4-ETH` | — | 建置；板名 / `<chip>/<board>` / project 名都可以 |
| `make.py build S3/ESP32-S3-Matrix --minimal` | — | 最小映像（只印心跳），新板 bring-up 用 |
| `make.py build ESP32-S3-N16R8 --tests` | — | **測試映像**（`NC_ENABLE_TESTS=ON`：`0xF00x` 指令與 test_* 任務）。不寫＝出貨映像（OFF）。每次都會明確傳給 CMake，不再靠 cache 記住上一個值 |
| `make.py build ESP32-P4-ETH --fresh` | — | 先刪這個 build dir 的 `sdkconfig` 再建（改了 defaults 才需要） |
| `make.py build ESP32-P4-ETH --jobs 4` | — | 限制平行編譯 |
| `make.py build --all` | — | 依序建置**全部**板子（每塊一個 build dir，可重入；`--project`／`--no-project` 會被拒絕，因為每塊板子用哪個 project 由 hardware 層決定） |
| `make.py projects` | — | 列出每塊板子的 project（`default` 會標出來） |
| `make.py check [板子]` | — | 驗 `board.json` ↔ `sdkconfig.defaults` 一致（同 `tools/board.py check`） |
| `make.py doctor` | — | 環境健檢：IDF / idf.py / esptool / pyserial / 板子設定 / 磁碟 / git |
| `make.py flash ESP32-P4-ETH [--full\|--erase]` | ✅ | 建置 + 燒錄（預設 `app-flash`，不動碟上的使用者資料） |
| `make.py monitor ESP32-P4-ETH` | ✅ | 序列埠監看 |
| `make.py outputs [--prune N]` | — | 列出歷史產物 / 每塊板子只留 N 顆 |
| `make.py config ESP32-P4-ETH` | — | 印出這塊板子的 board.json + 合併後的有效 Kconfig |
| `make.py clean [--all]` | — | 清 `artifacts/build/`、舊 `firmware/build*`／`sdkconfig*`；（`--all` 連 `managed_components`） |
| `make.py boards` / `list` / `ports` / `env` | — | 板子表 / 插著的板子 / 序列埠 / IDF 路徑 |

### 產物（`artifacts/`）

命名規則跟 `mp_Make-Tools` 一致：**只有撞名才加時間戳**。

```text
artifacts/
├─ ESP32-P4-ETH.bin                   最新 app image（OTA 用這個）
├─ ESP32-P4-ETH_2026_11_16_15_07_27.bin  上一顆退成歷史檔
├─ ESP32-P4-ETH.json                  尺寸 / sha256 / git / IDF 版本
├─ ESP32-P4-ETH/                      這顆的完整可燒 .bin 組
│  ├─ esp_net_core.bin
│  ├─ bootloader.bin
│  ├─ partition-table.bin
│  ├─ ota_data_initial.bin
│  ├─ flash_args
│  └─ manifest.json
├─ build/<chip>_<board>/              CMake / ninja / .o 全部在這裡（每塊板一個）
└─ logs/<board>_<時間>.log
```

> ⚠️ 與 `mp_Make-Tools` 的差異：它的時間戳貼在**新**產物上（`NAME.bin` 一直是最舊那顆）；
> 這裡貼在**舊**產物上。理由是 `NAME.bin` 必須永遠是最新的一顆 —— 否則
> `ota.py artifacts/<board>.bin` 會安靜地燒到第一次建置的舊韌體。

`firmware/` 不再有 `build/`、`build_p4/`、`build_p4c6/`、`sdkconfig`；所有中間檔都在
`artifacts/build/<chip>_<board>/`，`sdkconfig` 也在裡面（`-DSDKCONFIG`），所以多塊板
可以並存、不用每次切 target。`make.py clean` 清 `artifacts/build/` 與舊的
`firmware/build*`／`sdkconfig*`；`--all` 連 `managed_components`／`dependencies.lock`。

`--no-archive` 可關掉 `.bin` 收集；`make.py outputs --prune 3` 只留每塊板子最近 3 顆。

### 設定檔

`make_config.json`（root）：`default_board`、`build.jobs`、`build.keep`、
`build.output_dir`（預設 `artifacts`）、`build.build_dir`（預設 `<output_dir>/build`；
可指到專案外，例如 `~/.cache/esp_Net-Core/build`）、`idf.per_chip`、`serial.port`。
命令列參數優先。

### 跟舊 `board.sh` 的關係

`tools/board.sh` 現在只是 `exec python3 make.py "$@"` 的相容殼——**一個實作，不會兩邊漂移**。
`--console jtag|otg` 拿掉了：console 屬於板子，寫在 `board.json` 的 `console` 欄位。

`flash.sh` 仍然保留，用於「TinyUSB 佔住 USB PHY、`idf.py` 進不了 download mode」的情況：
它會等你按 BOOT、燒完再 watchdog reset 踢回 app。

### `board.py` — 板子設定的真相

板子清單不是寫在腳本裡，而是兩層：
`ports/hardware/<chip>/<board>/board.json`（硬體）＋
`ports/project/<chip>/<board>/project.json`（項目）：

```bash
python3 tools/board.py list      # 硬體板 × project × flash/PSRAM/ETH/console
python3 tools/board.py projects  # 每塊硬體板下面有哪些 project（default 會標）
python3 tools/board.py check     # 驗 board.json ↔ sdkconfig；擋下「把 build-time 設定寫進 config.json」
python3 tools/board.py sync      # 改了 ports/_profiles/ 之後，把能力碎片寫進 hardware 層的 sdkconfig.defaults
python3 tools/board.py resolve ESP32-P4-ETH        # 查「找不到板子」：解析到哪個 hardware／project／target（跟 make.py 同一套）
python3 tools/board.py new --chip S3 --board ESP32-S3-Matrix --flash 16 --psram 8 --wifi
python3 tools/board.py new --project demo --hardware P4/ESP32-P4-ETH --default
```

`--wifi` / `--eth` / `--usb` 這些是**能力碎片**（`ports/_profiles/`），寫進 hardware 層；
`--console`、`--flash`、`--psram` 是 L1 硬體事實。`--project` 建的 project 只放 runtime 設定
（`data/config/*.json`），**不會**帶 `sdkconfig.defaults` 或 profiles。

硬體 preset 與 project 的 `hardware.json` 快照要保持同步——改完跑：

```bash
python3 tools/sync_hardware_presets.py --check   # 只檢查（CI 用；有漂移就非 0）
python3 tools/sync_hardware_presets.py           # 重寫 project/data/config/hardware.json
```

新增板子**不需要改 `board.sh` 或 CMake**。`board.sh` 會自己問 `board.py` 這塊板子的
硬體目錄、project、idf target 與 IDF 路徑。細節見 [ports/README.md](../ports/README.md)。

## ② 對設備說話

| 工具 | 需要板子 | 做什麼 |
|---|---|---|
| `nc4.py --port … reboot\|status\|raw\|listen` | ✅ | 送單一 NC4 指令 / **查狀態（keys 可選）** / 監看輸出 |
| `ota.py --port … <image>` | ✅ | NC4 OTA 推 firmware，**不需要按鍵、不需要 download mode** |
| `selftest.py --port …` | ✅ | 一鍵自測：STATUS / 解碼器 9 case / 效能 / 突發 / OTA（會匯入同層的 `ota.py`） |
| `nc4_file.py --port … status\|query <path>\|claim\|release` | ✅ | 檔案查詢（0x2005）與儲存狀態（0x1103/0x1101）。讀取任何模式都通；只有寫入才要 claim，見 [fat-ownership](../docs/03_notes/fat-ownership.md) |
| `nc4_test.py --port …` | ✅ | 解碼器一致性測試：正常幀 / 黏包 / 拆包 / 壞 CRC / 假 SOF / 錯 addr / 超大 LEN / 錯版本 |
| `buf_test.py --port …` | ✅ | **統一緩衝（`nc_buf`）的上板自測**（`0xF032`）：64 B 對齊、FIFO 次序、滿了回 NULL、reserve 表頭 + `off`/`len`、S2S·S2P·P2S·P2P 四條 copy、換手；`--status` 順便印 `mem.buf` |
| `ncm.py pack / dump` | ❌ | **打包 `.nct` / `.nce`**（header + manifest + meta + payload + 指令表）與檢視；`--meta meta.json` 嵌入 name/desc/usage/requires/provides。`pack` 會先擋掉裝置端一定會拒的值 |
| `ncm_build.py [task…] [--device]` | 裝機時 ✅ | **外掛模組獨立編譯工具**：WAT／WASM（C 預留）→ `out/*.nct／*.nce`；讀 `modules/<role>/<name>/build.json`（fps／budget／mem／cmds…）；leaf 名要跟 role（`_tasks`／`_actions`／`_effects`／`_lab`）；中間 `.wasm` 放 `modules/build/`、成品放 `modules/out/`；`--dir` 可指向外部專案；`--list`、`--clean`、`--device`（直接裝到 MSC `tasks/`） |
| `matrix.py --port … stats\|effect\|dither\|color\|gain\|fps\|zone\|zmask` | ✅ | **8×8 WS2812 矩陣**（`0xF060`）：切效果（第一區／呼吸／波浪／單色／色階）、抖動開關、每區 rate+phase、看實測 fps／late／tx_us。見 [rgb-matrix-dither](../docs/03_notes/rgb-matrix-dither.md) |

firmware image 在 `artifacts/<board>.bin`（先 build 才有；完整可燒組在 `artifacts/<board>/`）。

## ③ `bench/` — 量測（要板子，多數要 P4 乙太網）

| 工具 | 量什麼 |
|---|---|
| `bench_all.py` | 一次跑完 A–F 六組：純 TCP / NC4 下載 / NC4 上傳 / 純 CPU / 雙核 hub / 匯流排統計 |
| `net_bench.py` | 純 TCP 吞吐（port 5000/5001/5002），驗網卡上限 |
| `nc4_tcp.py` | NC4 over TCP 吞吐與延遲（port 6000） |
| `nc4_ws_test.py` | NC4 over WS 全套：RTT / 吞吐 / 碎片 / 重連 / 並存 |
| `nc4_udp_test.py` | NC4 over UDP：RTT / 吞吐 / 掉包 |
| `nc4_stats.py` | 0xF008 匯流排統計（`copied` 應為 0）＋ 碎片/垃圾重同步邊界 |
| `nc4_tx_bench.py` | 0xF007 上傳（打包＋送出），同核 / 分核 |
| `hub_bench.py` | 0xF003 雙核 hub 交換量測 |
| `buf_probe.py` | **緩衝貼身化**：`snap` / `sizing` / `session` / `compare` / `rtt` / `uart` / `txbench` / `storm` / `idle`。`session` 跑標準量測並輸出 JSON、`compare` 並排兩次並標 >3% 退步；`sizing` 把峰值換算成建議尺寸（已知但沒打出來的需求用 `--declare CDC=4096` 補）。流程見 [measurement-method.md](../docs/02_guides/measurement-method.md)，本次紀錄見 [buffer-tightening.md](../docs/03_notes/buffer-tightening.md) |

P4 預設 **DHCP**（舊版寫死的 `10.10.1.50/16` 已移除）；量測前先用 `nc4.py --port … status`
或序列埠看它拿到哪個 IP（`network.json` 的 `lan.ip.mode=static` 目前**未實作**）。

## ④ `hosttest/` — 主機端 C 測試（不需要板子）

用 `cc` 直接把同一個 `.c` 編進來跑，驗證純邏輯，不用燒板子：

```bash
bash tools/hosttest/run_all.sh     # 一次跑全部（12 套；每套印 OK / FAIL）
tools/hosttest/hub_test/run.sh     # nc_hub.c：格子交換 / 自動歸還
tools/hosttest/plan_test/run.sh    # nc_plan.c：宣告 / commit / 重名
tools/hosttest/mod_test/run.sh      # nc_mod.c：載入 / 選 / 兩幀釋放 / trap / 超支 / 掃描→合併→載入
tools/hosttest/ncd_test/run.sh     # `.nct` / `.nce` manifest：解碼 / 範圍 / 指令表
tools/hosttest/ncm_test/run.sh     # ncm.py：pack / dump / 副檔名 / magic / manifest / crc（格式標準）
tools/hosttest/task_test/run.sh    # 任務管理器：level 排序 / 帳本 / attach / detach / provides / config
tools/hosttest/cfg_test/run.sh     # nc_cfg_patch：token patch（巢狀 / 陣列 / 轉義）
tools/hosttest/cfgstruct_test/run.sh  # nc_cfg_load／defaults：三層 config 疊加、線的 kind/index、USB personality
tools/hosttest/router_test/run.sh  # nc4_router.c：線 → 核、out 轉寄清單（decodeN／自我／重複）
tools/hosttest/link_test/run.sh    # nc4_link.c：出口表、整幀送出、短送／錯誤 → drops、JSON
tools/hosttest/buf_test/run.sh     # nc_buf.c：格狀態機 / FIFO / reserve / 32-bit / S·P 記帳 / 換手 / JSON 截斷
tools/hosttest/portgen_test/run.sh # ncportgen.py：從 ports/ 產生 nc_port.c/h（含 USB kind 表）
```

Windows（MSVC）用同目錄的 `run.bat`。目前 `cfg_test` / `ncd_test` / `mod_test` / `ncm_test` /
`router_test` 有可用的 `run.bat`；`task_test` 的 `run.bat` 需要 GCC/Clang（`nc_buf.c` 用
`__ATOMIC_*`），Windows 請用 WSL 的 `run.sh`。`mod_test` 的 `dirent.h` 由
`tools/hosttest/mod_test/include/dirent.h` 補上（Win32 `FindFirstFile` shim），
`clock_gettime` 用 `QueryPerformanceCounter` 代替。`ncm_test` 是 Python（`ncm.py` 的格式標準測試）。
其餘只是還沒補腳本（純 C，補了就能跑）。`cfgstruct_test`／`portgen_test` 直接吃 `firmware/`
的標頭與 `ports/`，在 WSL／macOS 跑最省事。

全部都會印 `ALL PASS`。改了 `sys/mem/`、`sys/task/`、`sys/task_manager/`、`sys/config/`、`tasks/test_mod_tasks/` 之後先跑對應那支。

## ⑤ `poc/` — 原型驗證

| 工具 | 需要板子 | 做什麼 |
|---|---|---|
| `poc/mod_poc/host.mjs` | ❌ | 效果模組 ABI 的 host 參考實作（node，零依賴）：跑模組、中途換模組、餵壞模組 |

```bash
node tools/poc/mod_poc/host.mjs tools/poc/mod_poc/mod.wasm --frames 120
```

---

## 還沒有的工具

文件提到但**目前不存在**，屬於計劃產物，不要當成壞掉的路徑：

- `tools/fixtures/` — 測試樣本（[component-layout.md](../docs/03_notes/component-layout.md) §8 規劃）
