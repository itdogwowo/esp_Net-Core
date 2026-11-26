# 雷霆大文件 — esp_Net-Core 專案總覽（所有 AI 必讀）

> **這份是咩**：呢個 repo 的 AI onboarding 總文件。任何人（AI 或新維護者）第一次接觸
> 這個專案，**先由頭讀到落尾**，再開始改任何東西。細節文件在 `docs/`，但方向、鐵律、
> 工作流程、最常踩的坑全部在這一份。
>
> **最後更新**：2026-11-26（新增 §0.6 測試環境 `tenv`；版本統一提醒、鐵律 12、host test 21 支）
> **維護規則**：每次架構／流程／工具改變，先改這份；過期內容直接覆蓋，唔好累積兩套講法。

---

## 0. 30 秒摘要

- **專案**：ESP32-P4（另有 ESP32-S3 port）上的 **NC4 通訊核心**：多條 transport →
  分核解碼 → 指令執行；外加 **pixel 效果 → 抖動＋配對 → 編碼 → 推送 → I80／SPI／RMT／PARLIO 送燈** 管線。
- **語言／工具**：C17、ESP-IDF（macOS／Windows `.idf`／WSL **全部 v6.1.1**；見 §0.5，
  **三個環境版本要統一**）、FreeRTOS、`python3 make.py`、host 測試（唔需要板）。
- **兩個世界**：
  1. **NC4／緩衝／任務／網絡**：協議、通道、分核、`nc_buf`。
  2. **Pixel**：12-bit 效果 → 抖動／配對 → 8-bit 環 → I80／SPI 後端。
- **板子差異**在 `ports/hardware/`，**跑咩**在 `ports/project/`，**runtime 設定**在
  裝置 FAT 的 `/data/config/*.json`。
- **平時更新 firmware 用 USB NC4 OTA**（`tools/ota.py`），**唔使** download mode／
  唔使叫用家按 BOOT。詳見 §2。

---

## 0.5 快速開始：環境、更新、測試

### 環境（三個地方；**版本一定要統一**）

| 環境 | 用途 | ESP-IDF | IDF 路徑 | 工具鏈 |
|---|---|---|---|---|
| macOS（Apple Silicon） | 主要開發／量測 | **v6.1.1** | `~/esp/esp-idf` | `~/.espressif` |
| Windows | 這台機的 repo `.idf/` | **v6.1.1** | `<repo>/.idf/esp-idf` | `<repo>/.idf/tools`（Windows exe） |
| WSL Ubuntu 24.04（20 核） | 跑 host test／build | **v6.1.1** | `/home/user/esp/esp-idf` | `~/.espressif`（Linux） |

- Python 3.12；建置入口 `python3 make.py`（**不要**直接跑 `idf.py`）。
- 板子：ESP32-S3（4 MB flash／2 MB PSRAM；USB CDC）；另有 P4／C3 port。
- ⚠ **三個地方版本唔同 = 兩個唔同的 image**（統一前實測同一份 code：Matrix 差 9,584 B、
  P4-ETH 差 24,688 B）。2026-11-25 起三個環境都係 **v6.1.1**；記錄仍然要寫
  `idf=` 同**邊個環境**（見 §0.5「版本一致性」與鐵律 12）。

### Windows 主機：先確認 PATH 上的 `bash` 係邊個

`make.py`／`doctor`／host test 全部用 `bash -c` 跑，而且會傳 **Windows 絕對路徑**
（`cd "<repo>\firmware"`、`-B "<repo>\artifacts\build\…"`）。所以：

| PATH 上的 bash | 會發生咩 | 點做 |
|---|---|---|
| **Git Bash**（MSYS） | bash 讀唔到 `C:\…`（會當成檔名）→ build 前置就失敗；`doctor` 的 board check 會**誤報 FAIL**（顯示「0 個錯誤」）、pyserial 也讀不到 | 改用 WSL：見下一行 |
| **WSL 的 bash**（`C:\Windows\system32\bash.exe`） | Windows 側下指令傳 `C:\` 進去一樣失敗 | 建置／host test **在 WSL 內跑**：<br>`bash -lc 'cd /mnt/c/<repo> && IDF_PATH=/home/user/esp/esp-idf python3 make.py build …'`<br>OTA／NC4／board 用 Windows 側 `python3 tools/…` ＋ `COM*` |

- 2026-11-26 實測：這台 Windows 機是後者（repo `.idf/esp-idf` 也是 v6.1.1，但 Windows 側
  `make.py` 的 bash 指令帶 `C:\` 一律失敗）。建置記錄一樣要寫 `idf=v6.1.1 (WSL)`。
- 判斷方法：`bash -c 'uname -a'`（見到 `microsoft-standard-WSL2` 就是 WSL）＋
  `(Get-Command bash).Source`。
- **版本要小心 `-dirty`**：Windows 的 git 通常 `core.autocrlf=true`（checkout 成 CRLF）；
  WSL 的 git 預設冇設，會把整棵樹當成「改過」→ WSL 內 build 的 `PROJECT_VER` 永遠帶
  `-dirty`（2026-11-26 實際撞到：乾淨樹出 `fw=61293b7-dirty`）。
  修法：在 repo 內 `git config core.autocrlf true`（Windows／WSL 共用同一份 `.git/config`），
  之後 `git describe --dirty` 在兩邊一致。
- 這台機的實際值記在 `.testenv/machine.json` 的 `notes`（`tenv set notes …`），
  不進 git。


```bash
python3 make.py doctor          # IDF／esptool／板子／磁碟
python3 make.py ports           # /dev/cu.usbmodem*
python3 make.py projects        # 板 × project
```

> `make.py doctor` 如果話找不到 `.idf/esp-idf`，係因為它預設看 repo 內的路徑；
> 用 `IDF_PATH=$HOME/esp/esp-idf` 或在 `make_config.json` 設 `idf.path`。

### 更新 firmware（USB NC4 OTA；唔使 BOOT）

```bash
# 1) 建置
IDF_PATH=$HOME/esp/esp-idf python3 make.py build ESP32-S3-Matrix \
    --project ESP32-S3-Matrix-LED-Textkit

# 2) OTA 推上板（image 在 artifacts/<board>__<project>.bin）
python3 tools/ota.py --port /dev/cu.usbmodemXXXX \
    artifacts/ESP32-S3-Matrix__ESP32-S3-Matrix-LED-Textkit.bin

# 3) 驗證
python3 tools/nc4.py --port /dev/cu.usbmodemXXXX status sys
python3 tools/nc4.py --port /dev/cu.usbmodemXXXX status tasks
```

- 預期：`BEGIN` 2–4 s（同步抹分區）→ `WRITE` 約 150 KB/s → `END` 驗 SHA →
  `APPLY` 重啟；`status sys` 的 `app` hash 會變、`rst=3`。
- OTA 失敗先 `tools/ota.py --port <PORT> --abort`，再重推。
- **唔好**因為 `esptool` 連不上就叫用家按 BOOT；見 §1 鐵律 1。

### 做測試

```bash
# 主機測試（唔需要板；21 支，全部 -Werror）
bash tools/hosttest/run_all.sh

# 上板 smoke
python3 tools/nc4.py --port <PORT> status sys
python3 tools/nc4.py --port <PORT> status tasks
python3 tools/nc4.py --port <PORT> status mem

# render 統計（要 tests 映像；NC_ENABLE_TESTS）
python3 tools/nc4.py --port <PORT> raw 0xF061 00

# 配對／I80 A/B（要 EffectPerf 映像；一次性 lab 工具）
python3 modules/lab/i80_pack_lab/run_ab.py --port <PORT> --px 9600 --variant 17 --reps 8

# 效果層量測（EffectPerf 映像）
python3 tools/effect_bench.py --port <PORT>
```

- Show 映像要 `pixel_effect_task`／`pixel_paint_task`／`pixel_encode_task`／`render`
  全部 `level=4`、`started=1`、`running=1`；否則 `fs_format` 重 seed。
- 量測要記錄：同一塊板、同一映像、reps、min/median、`bus_ok`／checksum。
- 冇板就明講「未上板」，唔好把 host 數字外推。

### 版本一致性（每次測試都要對）

- **只有一個韌體版本來源**：`PROJECT_VER`（build 時 `git describe --tags --always --dirty`）。
  以下三者必須一樣：
  1. `status sys` 的 `fw`
  2. OTA `0x2207` 回的 version
  3. image app descriptor 的 version（`tools/ota.py` 的 `version:` 行）
- `make.py` 每次 build 都明確傳 `-DPROJECT_VER=...`，所以 build dir cache 不會
  沿用舊版本（今次真係撞過：同一份 code，EffectPerf 係 `197e1e0`、release 係
  `197e1e0-dirty`）。
- `tools/ota.py` 預設讀 image 的 app descriptor；要例外覆寫用 `--fw-ver`。
- 每次測試記錄：image path＋sha256、`fw`、`app`、`idf`、板 MAC；
  A/B 一定要同一 image、同一板。
- **IDF 版本以 `idf.py --version` 為準，而且三個環境要統一**（用家 2026-11-25 明確要求）：
  - 現時：macOS `~/esp/esp-idf`、Windows `.idf/`、WSL **全部 v6.1.1**（2026-11-25 實測
    `git -C ~/esp/esp-idf describe --tags`；見 §0.5 環境表）——**已統一**。
  - 唔同版 = 同一份 code 出兩個 image（實測 Matrix 差 9,584 B、P4-ETH 差 24,688 B），
    **數字唔可以互相對比**。
  - 升級／降級要**一次過升齊**；未升齊之前，每次記錄都要寫 `idf=` 同喺邊個環境跑。
    記錄格式：`idf=v6.1.1 (WSL)`。
  - `v6.1.0` 這個 tag **不存在**（只有 `v6.1`／`v6.1.1`）；以 `git describe --tags` 為準。

### 0.6 測試環境：每台機唔同（開工第一件事）

同一個 repo 會喺 macOS／Windows／WSL 之間走，三邊的 IDF、有冇板、板上 image／task 都
唔同。**唔可以靠記憶或上一節的版本表當成「現在」**；先跑：

```bash
python3 tools/tenv.py brief        # 一頁：IDF／板／image＋邊條 lane 跑得
python3 tools/tenv.py plan -v      # 每條 lane 的前置、指令、閘門
```

- 三層：`tools/testenv/policy.json`（團隊政策，進 git）／`.testenv/`（這台機的
  `machine.json`＋`last-probe.json`，**唔進 git**）／每次 probe（IDF、serial、
  `status cmds`、`status tasks`、`status ncm`）。
- `runnable` 才照 `command` 跑；`blocked`／`unknown` **唔等於測過**，唔可以寫「應該得」。
- 回報任何數字前貼 `python3 tools/tenv.py stamp`（`env=`／`idf=`／`board=`／`cid=`／
  `fw=`／`app=`／`img=`）。跨 IDF 版本數字唔可以互相對比（鐵律 12）。
- 第一次在這台機：`python3 tools/tenv.py init --label "…"`；接板後
  `python3 tools/tenv.py bind <board> [--project …]`。主機型機器（WSL／CI）：
  `python3 tools/tenv.py set absent-ok true`。
- 完整協議（狀態語義、lane 表、`machine.json` schema、故障排查）：
  [`docs/02_guides/test-environment.md`](docs/02_guides/test-environment.md)。

---

## 1. 使用者反覆提醒的鐵律（先讀這一節）

> 以下係用家最常要提醒 AI 的幾件事。**違反任何一條，就算功能「做到」都算做錯。**

### 鐵律 1：更新 firmware 用 USB NC4 OTA，唔好叫用家按 BOOT

**正常情況**（99%）：

```bash
# 1) 建置（會自動出 artifacts/<board>__<project>.bin）
IDF_PATH=$HOME/esp/esp-idf python3 make.py build <board> --project <project>

# 2) 用 NC4 OTA 推上板（唔使 download mode、唔使拆線）
python3 tools/ota.py --port /dev/cu.usbmodemXXXX \
    artifacts/<board>__<project>.bin
```

- OTA 走 **NC4 0x22xx**：`BEGIN`（同步抹目標分區）→ `WRITE`（4 KB chunk）→
  `END`（驗 SHA-256）→ `APPLY`（寫 boot 旗標＋重啟）。
- 實測速度約 **150 KB/s**；663 KB image 的 `BEGIN` 約 2.9 s、`WRITE` 約 4.3 s。
  `BEGIN` 等幾秒係正常，唔係死機。
- 推完之後 port 可能短暫消失再出現；用 `status sys` 確認 `app` hash 變了、
  `rst=3`（software reset）係正常。
- 中斷／超時：先 `python3 tools/ota.py --port <PORT> --abort` 清 session，
  再推。否則下一次 `BEGIN` 會回 `err_begin_partition_conflict`。
- 只想驗證唔切換：加 `--no-apply`。

**為什麼唔用 esptool／`make.py flash` 做日常更新**：

- S3 的 app 跑 **TinyUSB composite（MSC+CDC）**，唔會實作 Espressif download mode
  的 reset 序列。`esptool` 會回 `No serial data received`——**呢個係預期，唔係壞板**。
- 只有以下情況才需要 download mode：
  - 換 bootloader／分割表／flash 大小；
  - OTA 推咗壞 image，板卡在 boot loop；
  - 第一次由完全空白／Arduino 韌體轉過來。
- 入 download mode 的方法（今次實測有效）：
  1. **拔 USB**；
  2. **按住 BOOT（IO0）**；
  3. **插回 USB**，保持按住 2–3 秒；
  4. 放開 BOOT。ROM 會用另一個 USB 介面（今次係 `/dev/cu.usbmodem111401`），
     `esptool`／`make.py flash` 就連得到。
- 之後：
  ```bash
  IDF_PATH=$HOME/esp/esp-idf python3 make.py flash <board> --project <project> \
      --port /dev/cu.usbmodemXXXX --full
  ```
  `--full` 會寫 bootloader＋分割表＋app；只換 app 可用 `app-flash`（`make.py flash`
  預設），但**分割表唔同就一定要 `--full`**。
- 文獻有寫 CDC 收字串 `DFU` 可以入 download mode；**今次未驗證**，唔好當主要方法。
  最可靠仍然係 BOOT＋插電。

> **AI 行為要求**：唔好一遇到 `No serial data received` 就叫用家按 BOOT。
> 先問「有冇 NC4 port？可唔可以 OTA？」；只有 OTA 都做唔到才要求 recovery。

### 鐵律 2：改完一定要跑全套 host 測試

```bash
bash tools/hosttest/run_all.sh
```

- 21 支測試（`cfg_test` … `fpsclock_test`），全部 `-Wall -Wextra -Werror`。
- 每次跑之前會 `rm -rf <test>/build`，所以 header 改動唔會被舊 binary 遮住。
- **唔好只跑你改的那一支**；共用 header／codec／config 的影響經常在別處爆。
- 測試失敗先修，唔好「先 commit 再算」。

### 鐵律 3：要真機數字，唔要估算

- 用家講過好多次：「唔好講估算，直接用 USB（OTA＋nc4）驗證」。
- 量測工具：
  - `tools/layout_bench.py`（0xF077，EffectPerf 映像）
  - `tools/effect_bench.py`（0xF065，EffectPerf）
  - `tools/nc4.py raw <cmd> <payload>`（任何 0xF0xx）
  - `tools/nc4.py status ...`（sys／tasks／mem／storage）
- 冇板／冇某個配置就**明講未量**，唔好把 host x86 數字外推成 S3 數字。
- 每次量測記錄：同一塊板、同一映像、reps、min/median、`bus_ok`／checksum 之類的
  正確性閘門。

### 鐵律 4：Pixel 管線零拷貝、唔清畫布、唔補黑

- 配對層直接寫「硬件即將讀取的環槽」：
  `nc_px_ring_begin()` → `nc_px_layout_apply*()` → `nc_px_ring_commit()`。
- **冇 canvas、冇 memset、冇「畫完再複製」**。開機只寫黑一次（初始化）。
- 用户寫什麼就顯示什麼；唔好加「自動黑畫面」「自動補黑」「每幀清畫布」。
- 搜尋 `memset(s_content.canvas` 應該係 0 筆。
- 呢個係定案，唔係 optimisation 建議；改動前先讀
  [`docs/00_pm/pixel-tiers-plan.md`](docs/00_pm/pixel-tiers-plan.md) 的「已定案」。

### 鐵律 5：文件要同步，`status.md` 係快照唔係日誌

- 完成一個 Task：勾計劃的 `- [x]`，並在 `docs/00_pm/status.md` 加一節（§7.x）。
- **`status.md` 係快照**：舊數字直接覆蓋，唔好把過期數字堆在同一頁；要留歷史用 git。
- 新設計／量測 → `docs/03_notes/`；協議 → `docs/01_protocol/`；操作 → `docs/02_guides/`。
- 用戶看的文件用**繁體中文／粵語書面**；code comment 也一樣，解釋「為什麼」。

### 鐵律 6：換 project／config 舊了，用 `fs_format` 重新 seed

換 project 後 `/data/config/tasks.json` 可能仍是舊 level（`render`／`pixel_encode_task = -1`），
板子起來但唔點燈。處理：

```bash
python3 tools/nc4_file.py --port <PORT> set '{"System":{"fs_format":1}}' 1
python3 tools/nc4.py --port <PORT> reboot
```

- 會**清空 `/data`** 並重建 FAT，再由 project 的出廠預置重新 seed。
- 只在「換 project／config 壞／要出廠狀態」時用；一般更新 firmware 唔會清 `/data`。
- 之後用 `status tasks` 確認四條 pixel 任務 `level=4`、`started=1`、`running=1`。

### 鐵律 7：用 `make.py`，唔好自己亂跑 `idf.py`

- 建置入口：`python3 make.py build <board> [--project <name>] [--tests] [--fresh]`。
- 每塊板有自己的 build dir／sdkconfig：`artifacts/build/<chip>_<board>/sdkconfig`。
- 改 `sdkconfig.defaults` 後唔生效 → `--fresh`（只清這塊板的 build dir）。
- 唔好把 `firmware/sdkconfig` 留低；`make.py clean` 會清舊物。
- 建置前可跑 `python3 make.py doctor`、`python3 make.py projects`、`python3 make.py ports`。

### 鐵律 8：Decode handler／中斷路徑唔好放大的 stack buffer

- `decode0` 任務 stack 只有 **3,072 B**；`main` 預設 3,072 B（Show 類 project 覆寫成 8,192）；`pixel_encode_task` 4,096 B、`render`（push）3,072 B。
- 任何 handler 的 JSON／大暫存用 `static` 或 heap，**唔好** `char j[1200]` 放 stack。
- 2026-11-24 真的爆過：`on_layoutbench` 的 `j[1200]` 令 frame 2,272 B，
  任何 `0xF077` 都 panic。改成 `static` 後 1,168 B 正常。
- 同類教訓見 `nc_effect_bench.c`、`nc_file_action.c` 的註解。

### 鐵律 9：唔好靜默降級／靜默截斷

- 記憶體唔夠、表太大、欄位超限：**明示失敗**（`skipped++`、`err_*`、log），
  唔好靜靜行另一條更慢的路或截斷資料。
- 例：`nc_px_layout_flatify()` 只准 internal；唔夠就唔建表、行 runs，
  唔會靜默放 PSRAM（放 PSRAM 實測慢過 runs）。
- 例：舊 `nc_px_plan_t` 容量唔夠會計 `skipped`，唔會靜靜 drop。

### 鐵律 10：未經同意唔好 push

- 可以本地 commit（repo 文化係每個 Task 一個 commit），但 `git push` 前要問。
- Commit message 用繁中，講「為什麼」＋量測數字；唔好只寫 "fix"。
- 改完先 `git diff --check`、`git status`，確認冇 build artifact／暫存檔。

### 鐵律 11：一次性工具放 `modules/lab/`，唔好污染主體

- **一次性實驗／量測** → `modules/lab/<name>_lab/`：
  - 一個資料夾＝README（目的／結果／怎麼跑）＋腳本或 patch；
  - 量完可以整包移除；數字記錄在 `docs/00_pm/status.md`／`test-register.md`。
- **`tools/` 只放永久主機工具**（`nc4.py`、`ota.py`、`layout_bench.py`…）；
  lab 專用 variant／一次性流程唔好加進去。
- **`firmware/` 只放 production／driver／正式 bench**；`components/bench/` 係
  test-only，但仍然是 firmware 樹，只放「需要跟 firmware 一起編」的量測。
- 本次 Task 6 的 `0xF077` variant 17／18 就係 lab instrumentation：
  firmware 側留在 test-only bench，host 側 orchestration 搬去
  `modules/lab/i80_pack_lab/`；`tools/layout_bench.py` 保持通用。
- 16-lane 驗證完，firmware 側的 lab variant 可以移除（或留 patch 在 lab）。

### 鐵律 12：版本要統一（IDF ＋ 韌體）

- 同一個 repo 有**三個環境**（macOS／Windows `.idf`／WSL），**IDF 版本必須一致**。
  現時 macOS／Windows `.idf`／WSL **全部 v6.1.1**（已統一；見 §0.5 環境表）。
- 升級／降級要**一次過升齊**；唔好長期兩版並存，更唔好一邊 6.0 一邊 6.1 去比數字。
- 記錄任何 build／量測／OTA 之前，先寫 **`idf=`**（`idf.py --version`）同**邊個環境**：
  `idf=v6.1.1 (WSL)`。冇寫 = 呢個數字唔可以同其他數字比較。
- 統一前實測：同一份 code，6.0.3 vs 6.1.1 的 image 差 **Matrix +9,584 B、P4-ETH +24,688 B**
  （歷史數字；而家三個環境同版，唔再有這個差）。
- 韌體版本仍然只有一個來源：`PROJECT_VER`（見 §0.5），`status`／OTA／image 三者要一樣。
- 部機**冇板**時唔緊要（build 唔需要板），但 `idf=` 唔可以漏。

---

## 2. 這個專案是什麼（給第一次接觸的人）

### 2.1 一句話

**NC4** 是一條「多通道 → 分核解碼 → 指令 → 回應」的通訊核心；**Pixel** 是建在它上面的
一條即時管線：效果層算 12-bit 值 → 配對層寫環 → 編碼層出該硬件的線材格式
（I80／SPI／RMT／PARLIO）→ 輸出層送燈。

### 2.2 兩個子系統

| 子系統 | 做什麼 | 主要位置 |
|---|---|---|
| **NC4** | USB／UART／TCP／UDP／WS／ESP-NOW／I2C／SPI／CAN 收幀；CRC；分核 decode；router；回覆 | `firmware/components/sys/nc4/`、`main.c` |
| **Pixel** | 效果（program interpreter）→ 抖動＋配對（layout）→ 編碼（encode，多種硬件格式）→ 推送 → I80／SPI／RMT／PARLIO → 燈 | `firmware/components/sw/pixel/`、`tasks/pixel_effect_task/`、`tasks/pixel_paint_task/`、`tasks/pixel_encode_task/`、`tasks/render_task/`（公用）、`driver/drv_i80/` |

### 2.3 不是什麼

- 不是 Linux／POSIX 應用；冇 file system API 之外的抽象。
- 不是「全部 config 都可以 runtime 改」：build-time（L1/L2）與 runtime（L3）分開，
  見 §9。
- 不是「所有板子都有同一組 capability」：ETH／WiFi／PSRAM／USB 人格由板子決定。

---

## 3. 目錄地圖（去哪裡找什麼）

```text
esp_Net-Core/
├─ AGENTS.md                 ← 你在這裡（AI 總文件）
├─ README.md                 人類入口／目錄地圖
├─ make.py                   建置入口
├─ docs/
│  ├─ 00_pm/                 現況、計劃、量測、路線圖
│  │  ├─ status.md           ★ 單頁現況（數字＋已定案＋未做）
│  │  ├─ pixel-tiers-plan.md ★ pixel 三層落地計劃（Task 1–8）
│  │  ├─ effect-perf.md      EffectPerf 專案與量測
│  │  └─ night-report-*.md   夜間執行報告
│  ├─ 01_protocol/           frame、commands、transports
│  ├─ 02_guides/             build/flash、testing、configuration、connectivity
│  ├─ 03_notes/              架構／設計／量測筆記（task-model、buffers、config-layers…）
│  └─ 04_evolution/          進化日誌（tech-evolution ★ 全樹 9 條主線、pixel-evolution ★ 專文）
├─ firmware/
│  ├─ main/main.c            開機 B01–B11、USB、router 裝配
│  ├─ components/sys/nc4/    framing、decoder、bus、router、link
│  ├─ components/sys/task*/  任務模型＋管理器
│  ├─ components/sys/mem/    nc_buf、plan arena
│  ├─ components/action/     file／ota／test 指令表
│  ├─ components/sw/pixel/   環、codec、layout、engine（純 C）
│  ├─ components/tasks/      pixel_effect_task、pixel_paint_task、pixel_encode_task、render_task（公用輸出層）
│  ├─ components/effects/    內建效果
│  ├─ components/driver/     drv_i80、drv_spi_led
│  └─ components/bench/      0xF065／0xF077 量測
├─ ports/
│  ├─ hardware/<chip>/<board>/   board.json／wiring.json／driver.json／sdkconfig
│  ├─ project/<chip>/<board>/    project.json＋data/config＋data/pixel
│  └─ _profiles/                 能力碎片（eth-rmii、nc4-ws…）
├─ tools/                    主機工具（nc4.py、ota.py、layout_bench.py、hosttest/…）
├─ web_tools/                ESP WebMaster（REST／WS／檔案／OTA）
├─ modules/                  外掛 .nct／.nce ＋ lab/ 一次性實驗
└─ artifacts/                建置產物（gitignore）
```

**找東西的優先次序**：

1. 想知「現在點」→ `docs/00_pm/status.md`
2. 想知「下一個 Task」→ `docs/00_pm/pixel-tiers-plan.md`（或 `roadmap.md`／`next-steps.md`）
3. 想知「協議」→ `docs/01_protocol/commands.md`、`frame.md`
4. 想知「點 build／flash／test」→ `docs/02_guides/`
5. 想知「為什麼這樣設計」→ `docs/03_notes/`
6. 想知「為什麼變快／每一步試過什麼／失敗過什麼」→ `docs/04_evolution/`
   （`tech-evolution.md` 全樹 9 條主線；`pixel-evolution.md` pixel 逐階段細節，含 8×8 SWAR 圖解）

---

## 4. 開機流程 B01–B11（main.c）

| 階段 | 名字 | 做什麼 | 失敗的意義 |
|---|---|---|---|
| B01 | banner+nvs | 印 banner、開 NVS | NVS 壞 → 後面全部受影響 |
| B02 | storage+cfg | 掛 FAT／WL、boot record、seed 出廠 preset、讀 config | 失敗 → config 唔 give，B03 之後 SKIP；B11 仍跑 |
| B03 | capability | 核數收斂、USB 人格（OTG vs USJ）由晶片＋config 定 | 人格錯 → 冇 USB／燒錄／console |
| B04 | hwcheck | claim 範圍檢查（SDIO／ETH／PIN／PWM／UART） | 腳位衝突 |
| B05 | hardware | 安裝 driver（PWM、ETH enable…） | 硬件起不來 |
| B06 | declare | 登記任務、讀 `tasks.json` 的 level／affinity | 任務唔 declare |
| B07 | commit | 資源 commit（`nc_buf` arena／plan） | 記憶體不足 |
| B08 | start | `on_start()`、按 layer 開 loop | 任務起不來 |
| B09 | usb | USB service（MSC／CDC） | 冇 USB 通道 |
| B10 | channels | net lines start（TCP／UDP／WS／HTTP）＋ bus | 冇網絡通道 |
| B11 | account | 印記憶體帳、boot 摘要 | 只影響報告 |

- Log 例：`boot[B02] storage+cfg 126.92 ms ok`；摘要 `B01 9.66 | B02 126.92 | …`。
- **B02 總閘**：storage+cfg 失敗 → CONFIG 唔 give；但壞 config ≠ storage 失敗，
  degraded 仍會開機（核心任務照跑）。
- 核心任務（`nc4`／`net`／`core`／NCM 引擎）**不受 `tasks.json` 管**；
  config 只管租戶。

---

## 5. 任務模型（房東／租戶）

- **核心（房東）**：`nc4`、`net`、`core`、NCM 引擎；`kind=NC_TASK_SYSTEM`；
  C 宣告，唔寫入 `tasks.json`。
- **租戶**：一般任務、`.nct` 載入的模組；`kind=NC_TASK_NORMAL`／`EFFECT`；
  `tasks.json` 管 `level`／`affinity`。
- `level=-1` = 唔啟動；同一 level 按註冊次序。
- `affinity=[core0,core1]`，只可以一個 1；`[0,0]` = 唔跑。
- **未設定 = `[0,0]`**：條目缺失／冇寫 affinity → `on_start` 照跑、loop 唔開。
- 缺席的租戶會自動補寫 `[0,0]`，**只加不改**。
- Pixel 四層四任務（2026-11-25 正名）：`pixel_effect_task`（效果值）→
  `pixel_paint_task`（抖動＋配對）→ `pixel_encode_task`（編碼）→ `render_task`（公用推送／渲染）。
  `render_task` order 10 先 on_start（建立環／lane／節拍），其餘按 order 跟；
  四條都要 `level=4`、`running=1`；少一條就唔點燈。舊名 `effect`／`pair` 由
  `nc_task.c` 的 alias 讀取（`render` 係現名），OTA 後不會被關掉。

### `.nct` / `.nce`

- `/data/pixel/effects/*.nce`（效果）、`/data/tasks/*.nct`（任務）。
- 掃到 → NCM 引擎 `nc_mod_load()` → 驗證 → 切 arena → `nc_task_attach()`。
- 改容器格式／manifest 先讀 `docs/03_notes/ncm-contract.md`、`ncm-loader.md`。

---

## 6. NC4 協議與主機工具

### 6.1 Frame

```text
"NC" | VER=4 | ADDR(2 LE) | CMD(2 LE) | LEN(2 LE) | DATA | CRC32(4 LE)
CRC 範圍 = VER..DATA（不含 SOF、不含 CRC）
```

- 回覆 JSON 上限 **2,600 B**；超過回 `{"err":"reply_too_big","need":N,"cap":2600}`。
- `STATUS_GET` 很多 key 會令回覆超過上限；要分批問。
- 詳細：`docs/01_protocol/frame.md`。

### 6.2 常用主機命令

```bash
ls /dev/cu.usbmodem*                      # 找 port；每次插拔可能改名
python3 make.py ports                     # 哪個 port 空閒
python3 tools/nc4.py --port <PORT> status sys
python3 tools/nc4.py --port <PORT> status tasks
python3 tools/nc4.py --port <PORT> status mem
python3 tools/nc4.py --port <PORT> status storage
python3 tools/nc4.py --port <PORT> reboot
python3 tools/nc4.py --port <PORT> raw 0xF061 00
```

- `status sys`：`app`（image hash）、`up_s`、`rst`、`heap`、`h_int`、`h_dma`、`h_ps`。
- `status tasks`：每任務的 `level`／`cfg_level`／`started`／`running`／`b`。
- `rst` 代碼：1=power-on、3=software、4=panic、5=int WDT、6=task WDT、11=USB。
- `status cfg`：parse 錯／預設數；`status storage`：FAT／MSC 狀態。

### 6.3 命令域

| 域 | 用途 | 權威 |
|---|---|---|
| `0x10xx` | system／reboot／status | `main.c`、`commands.md` |
| `0x11xx` | STATUS_GET／SET／ACK | `main.c` |
| `0x20xx` | FILE（FAT 上傳／查詢） | `file_action` |
| `0x22xx` | OTA（BEGIN／WRITE／END／APPLY／ABORT） | `ota_action` |
| `0xF0xx` | **測試／量測指令**（release 多數唔編） | `test_action`、各 bench |

- **`0xFxxx` 區段係測試域**：唔好把 production 功能加進去；新指令先查舊 schema，
  唔好開新域。
- 常用測試指令：
  - `0xF060 MATRIX`：效果生產者 stats／設定（pixel 任務）
  - `0xF061 RENDER`：render stats（**只在 `NC_ENABLE_TESTS` 映像**）
  - `0xF062 PXBENCH`：編碼吞吐（`CONFIG_NETCORE_PXBENCH`）
  - `0xF065 EFBENCH`：效果層量測（EffectPerf）
  - `0xF06B SHOWFRAME`：檔案驅動 show 的 live frame
  - `0xF077 LAYOUTBENCH`：配對／I80 合成量測（EffectPerf）
  - `0xF079 PIXDIAG`：pixel 記憶體／配置診斷（pair）

### 6.4 USB 的兩個世界

- **USJ／USB-Serial-JTAG**：ROM 的；download mode 用；console 可走這裡。
- **TinyUSB OTG composite**：app 的；MSC（FAT 隨身碟）＋ CDC（NC4／log）。
- 兩者共用 PHY，一次開機只會有一個人格；由 B03 決定。
- **MSC 開關**係 per-link：`system.json` 的 `links[].msc`（預設 0）。
- FAT 擁有權：host 掛載 vs device；寫檔前要 `file_mode=1`（`nc4_file.py claim`）。
- 詳細：`docs/03_notes/usb-layers.md`、`usb-config.md`、`fat-ownership.md`。

---

## 7. 建置（make.py）

```bash
# 環境
python3 make.py doctor
python3 make.py projects          # 板 × project
python3 make.py ports             # 連接的序列埠

# 建置（release）
IDF_PATH=$HOME/esp/esp-idf python3 make.py build ESP32-S3-Matrix \
    --project ESP32-S3-Matrix-LED-Textkit

# 建置（tests：多 58 條 0xF0xx，0xF061 只在這裡）
IDF_PATH=$HOME/esp/esp-idf python3 make.py build ESP32-S3-Matrix \
    --project ESP32-S3-Matrix-LED-Textkit --tests

# 改過 sdkconfig.defaults 唔生效
IDF_PATH=$HOME/esp/esp-idf python3 make.py build <board> --project <project> --fresh
```

- 產物：
  - `artifacts/<board>__<project>.bin`（**OTA 用這個**）
  - `artifacts/<board>.bin`（最新 app）
  - `artifacts/build/<chip>_<board>/`（build dir／sdkconfig）
  - `artifacts/logs/...`（建置 log）
- Project 選擇：
  - `ESP32-S3-Matrix-EffectPerf`：**只測效果**，輸出層關；`0xF065`／`0xF077`。
  - `ESP32-S3-Matrix-LED-Textkit`：內建效果＋render（production，1 lane I80）。
  - `ESP32-S3-Matrix-Show`：檔案驅動（pixel_effect／pair／wave／render），要 `/data/pixel` 四層資產。
  - `ESP32-S3-Zero-Show9600`／`Show2400`：9,600／2,400 px 驗收。
- `--tests` 會**覆蓋**同一 path 的 `.bin`；之後要 release 記得重建。
- 建置前確認 `IDF_PATH`：macOS `$HOME/esp/esp-idf`（**v6.1.1**）；WSL
  `/home/user/esp/esp-idf`（**v6.1.1**）——記錄要寫 `idf=`（見鐵律 12）。

---

## 8. 燒錄、OTA、回復（最重要的一節）

### 8.1 日常：NC4 OTA

```bash
python3 tools/ota.py --port /dev/cu.usbmodemXXXX \
    artifacts/ESP32-S3-Matrix__ESP32-S3-Matrix-LED-Textkit.bin
```

流程與預期：

1. `caps`／`state`：`idle=1`、`max_chunk=4096`。
2. `BEGIN`：裝置先 `esp_ota_begin()` **同步抹目標分區**，663 KB 約 2.9 s；
   工具 timeout = `10 s + size/100 KB`。
3. `WRITE`：4 KB chunk，stop-and-wait；約 **150 KB/s**。
4. `END`：`esp_ota_end()` 驗 SHA-256＋image；失敗回 `err_end_validate_fail`。
5. `APPLY`：設 boot 分區＋重啟；`rst=3`。
6. 驗證：`status sys` 的 `app` hash 變了、`up_s` 重新計、`status tasks` 正常。

失敗處理：

| 症狀 | 原因 | 處理 |
|---|---|---|
| `BEGIN` 回 `err_begin_partition_conflict` | 上次 OTA 中斷，state=writing | `tools/ota.py --abort`，再推 |
| `BEGIN` 等很久／no reply | erase 比估算慢 | 先 `status` 查 state；必要時加大 timeout |
| `WRITE` 中途斷 | USB 拔線／host timeout | `--abort` 後重推；image 已驗 SHA，唔會半截切換 |
| `APPLY` 後板唔起 | image 壞／分區唔夠 | 入 download mode，`make.py flash --full` 重燒 |
| `END` SHA 不符 | 檔案傳輸錯／讀錯 image | 重推；確認 image 與 build 一致 |

> **Rollback 未開**：`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` 冇開，推壞 image 會卡住。
> 所以 OTA 前要保留可入 download mode 的方法（BOOT 按鍵）。

### 8.2 Recovery：入 download mode

1. 拔 USB；
2. 按住 **BOOT／IO0**；
3. 插 USB，保持按住 2–3 秒；
4. 放開。port 會變成 ROM 的 USJ（今次係 `111401`，app 的 CDC 係 `24EC...`）。
5. 燒：
   ```bash
   IDF_PATH=$HOME/esp/esp-idf python3 make.py flash <board> --project <project> \
       --port /dev/cu.usbmodem111401 --full
   ```
6. 燒完拔插一次（唔好按 BOOT）返 app。

- `--full` = bootloader＋分割表＋app；換分割表／flash 大小一定要。
- app-only = `make.py flash` 預設（`app-flash`），保留 NVS／`/data`。
- `tools/flash.sh` 會自己找 ROM port＋watchdog-reset，但本專案日常用 OTA 就夠。

### 8.3 什麼時候**不要**用 OTA

- 換 bootloader／分割表／flash size；
- OTA 分區放唔落（image > slot，Matrix 係 1.5 MB）；
- 板卡在 boot loop／panic loop，USB CDC 起不來；
- 第一次由 Arduino／空白板轉過來。

---

## 9. 配置四層（改哪裡？）

| 層 | 檔案 | 換它要 rebuild？ | 例子 |
|---|---|---|---|
| L1 硬體事實 | `ports/hardware/<chip>/<board>/board.json`＋`sdkconfig.defaults` | ✅ | flash/PSRAM/CPU/ETH/console |
| L2 能力碎片 | `ports/_profiles/*.defaults` | ✅ | `eth-rmii`、`nc4-ws`、`nc4-udp` |
| L3 project 預置 | `ports/project/<chip>/<board>/data/config/*.json` | ❌（build 時 embed，但 runtime 可覆蓋） | 腳位、links、tasks |
| L3 live | 裝置 `/data/config/*.json` | ❌ | 用戶改過的值 |

- **`F.live` 永遠最高優先**；檔案存在就唔會被 seed 覆蓋（只建立、不覆蓋）。
- `fs_format=1` 會清 `/data` 並重新 seed。
- `wiring.json` 在 build 時生成 `nc_port.c`；改完要跑
  `python3 tools/sync_hardware_presets.py`（如有）。
- `tasks.json` 只管租戶；核心任務照 C 宣告。
- `system.json` 的 `links[].enable` 控制通道存在性；未 enable = 唔宣告緩衝、唔建 bus。
- WiFi **兩道閘**：build-time `CONFIG_NETCORE_WIFI`＋runtime `network.json wifi.enable`。
- `hardware.json` 已 typed：ETH／SDIO／PIN／PWM／UART／**I80／SPI**（2026-11-24）；
  `I2C`／`I2S`／`ENC`／`XL9555` 仍然未 parse。**`I80` 由 2026-11-24 起真的生效**：
  `/data/config/hardware.json` 的 `I80.list[]`（`data`／`wr`／`dc`／`cs`／`freq`；
  `bus_width` 由 `len(data)` 推出）開機由 `nc_cfg_apply_hardware()` 讀，改腳位唔使重 build。
- `driver.json` **只有 `LED` 段會被讀**（build 時 `ncportgen` → `nc_led_cfg_t`）；
  其餘段（TFT／SDcard／PCA9685…）仍然冇人讀。規矩：**driver 層唔准寫腳位**，
  腳位只在 hardware 層；`driver.json` 用 `bus:{type,index}` 引用。

---

## 10. Pixel 管線（效果 → 抖動＋配對 → 編碼 → 推送）

### 10.1 四層四任務＋環

```text
pixel_effect_task（core0，50 fps）
  └─ 每條 aprog evaluate program → 12-bit 值流（雙 pool＋ack，防撕裂）
        │  只傳指標（nc_pipe_effect_frame_t），唔複製值流
        ▼
pixel_paint_task（core1）
  └─ nc_px_layout_apply()／apply12()／apply4()／apply_dither()
        │  抖動在配對內圈（9,600 px 5.4 ms、零中間 buffer）
        ▼
環（i80.prd；64 B 前綴（24 B 表頭＋padding）＋px×4 B）        ← 零拷貝：直接寫硬件即將讀的格
        │
pixel_encode_task（core1，order 11）
  └─ 取最新格 → 該硬件格式（I80 bus word／SPI bit／RMT symbol／PARLIO word）
     → 入「格索引」隊列（8 格，唔 copy）
        │
render_task（公用輸出層；core1，order 10；環／lane／節拍的主人）
  └─ DMA 完成還格 → submit → I80／SPI／RMT／PARLIO → 燈
```

- 四個任務全部 `level=4`；`render_task` 先 on_start（建立環／後端／節拍），
  其餘按 order 跟。少一條就唔點燈。
- `pixel_effect_task`／`pixel_paint_task` 只在 `CONFIG_NETCORE_CONTENT=y` 存在；
  `pixel_encode_task`／`render_task` 屬 `CONFIG_NETCORE_PIXEL`，release 都有；
  `render_task` 係公用輸出層（LCD 將來共用），唔屬 pixel 系列。
- 後端支援矩陣：`i80`／`spi` 已實作；`rmt`（C 系列／S3）同 `parlio`（P4）嘅
  adapter／pins 仍然待做，見 [`pixel-output-backends.md`](docs/03_notes/pixel-output-backends.md)。
  `driver.json` 寫了未實作的 backend 會大聲報錯、停用 lane，唔會靜默退 mock。
- 環契約：`begin_ex()` 拿的那一格才寫；`commit()` 只發布同一格；
  encode 不可再 peek；**DMA 完成才 reclaim**（reclaim 在 push loop）。
  四段 handoff、三種「滿了」策略同待拍板問題見
  [`pixel-ring-flow.md`](docs/03_notes/pixel-ring-flow.md)。
- 舊 `pixel`（8×8 內建引擎）2026-11-25 **已退役**：它只是測試用生產者。
  舊 `effect`／`pair` 任務名由 `nc_task.c` 的 alias 讀取；`render` 係現名，
  OTA 後不會熄燈。

### 10.2 零拷貝／唔清畫布

- 舊路徑（已刪）：canvas 寫入 → 複製到環；每幀 memset。
- 現在：`nc_px_ring_begin()` → `nc_px_layout_apply()` → `nc_px_ring_commit()`。
- 開機只寫黑一次（init），之後每幀覆蓋；用戶寫什麼就顯示什麼。
- 冇「補黑」、冇「自動黑畫面」、冇隱藏行為。

### 10.3 配對表

- `nc_px_layout`：runs → 6 B 單表（`dst16/src16/write8/aprog8`）。
- 6 B 表**只准 internal**；唔夠就唔建表、行 runs（唔靜默降級去 PSRAM）。
- 9,600 px 的 6 B 表 57.6 KB；`flat_int` 可查位置。
- `apply`（8-bit）／`apply12`（12-bit）／`apply4`（4 子幀一次掃）／`apply_dither`（單子幀）。
- 重疊語義：dst 升序、後寫覆蓋先寫；唔可以按 write 分組（會改畫面語義）。
- 快取：`/pixel/cache/<key>.pxlt`，≤32 KB 才存；開機補建缺的。

### 10.4 I80 波形合成（Task 6，已接 production）

- 舊：每條 lane `wave_pack` 成 packed bitstream（16×600：16×5,400 B），再 `merge8` 轉置。
- 新：`nc_px_codec_pack_bus()` 每顆即時取 lane 位元組 → 8×8 SWAR 轉置 → 逐 tick 寫 bus word。
- 8-bit／無抖動：廣播快路徑；有 dither：完整轉置。
- **1 lane／8-bit 例外**：逐 tick 廣播比舊 `merge8` 慢，所以用
  `nc_px_codec_wave_frame()` 的 LUT memcpy（64 px：840 → 139–165 µs）。
- 實測（ESP32-S3、EffectPerf）：9,600 px 24,018 → **6,365 µs（3.77×）**；
  2,400 px 4,765 → **1,166 µs（4.09×）**；`bus_ok=1`。
- 16-lane × 16-bit 實機仍未做（冇板）；bench 係同一份 C 在板上跑。
- **pipeline 冇固定 32 上限**：aprog／frame 按 naprog 動態配置（40 個效果真機驗證）。
- **playlist 預建**：`registry.list` 次序預測下一個 mode，播放時預建 layout＋flat；
  切換 swap 實測 **246 µs**（原本 rebuild 40–150 ms）。
- **MSC 預設 0**：`nc4.links[].msc=1` 會把 FAT 交畀 USB host，之後上傳／刪檔全部
  `err_busy`。recovery：`CONFIG_NETCORE_USB_MSC_FORCE_OFF=y` 臨時唔交碟。
- **I80 有兩個實作、同一個 `nc_i80_drv_*` 契約**（2026-11-26）：`esp_lcd` 版（面板
  用，要齊 data＋WR＋DC）／**直驅版**（`CONFIG_NETCORE_PIXEL_I80_DIRECT`，只 route
  真正接燈的 lane，Matrix 只需 GPIO14 一支；`hal/lcd_ll.h`＋AHB GDMA 直接寫 LCD_CAM）。
  **設計紅線：唔可以把它異化成只for燈**——真熒幕（LCD 面板）能力要保留；直驅已完整
  LCD 化：`nc_i80_drv_submit_cmd(slot, lcd_cmd, bytes)`（cmd phase＋param phase）、
  `cmd_bits`、DC 四段電平、CS assert／de-assert；`wr`／`dc`／`cs` **全部可選**
  （≥0 才 route，唔填 -1 唔影響 LED），唔會像 esp_lcd 缺一就整條 lane 唔建立。
  面板**未有硬件驗證**（冇面板；cmd／DC 時序要示波器）。見
  [`pixel-i80-pin-schema.md`](docs/03_notes/pixel-i80-pin-schema.md) §8.4、status §7.255。
- 16-bit 高位 byte 用 store（唔用 OR），否則 lane 變暗會殘留。
- **每一步的歷史（含失敗實驗與數字）見
  [`docs/04_evolution/pixel-evolution.md`](docs/04_evolution/pixel-evolution.md)**；
  本節只講現況。

### 10.5 抖動

- `CONFIG_NETCORE_PIXEL_DITHER=0`、`DITHER_BITS=8` 預設：8-bit 直出。
- 4 子幀只用於 2,400 px（16×150）＋子幀率 ≥200 Hz；9,600 px 用單子幀。
- `apply4` 一次掃 4 份；production 逐子幀 `apply_dither`。
- **點開、要改幾多處、推導公式同物理上限**（`subframes / interval_ms / ring_slots / px/lane`
  四個數要一致，錯咗唔會響）→ [`03_notes/pixel-subframe-modes.md`](docs/03_notes/pixel-subframe-modes.md)。
- 環的取格政策：多子幀＝逐格 FIFO（背壓，四格全部送）、單子幀＝最新優先；
  見 [`pixel-ring-flow.md`](docs/03_notes/pixel-ring-flow.md) §2.3。

### 10.5b FPS 穩定度（時間緩衝）

- 節拍**仍然由 `esp_timer_start_periodic(frame_interval_ms)` 控制**；唔會追趕、唔改週期。
- `nc_fps_clock`：早完成存 slack、遲到由 slack 吸收；上限
  `CONFIG_NETCORE_PIXEL_SLACK_MS`（預設 50 ms，**初始全滿**）。
- 長期 `overrun` → 改效果或降 FPS，**唔好加大 cap**。
- 量測：`0xF081` → `0xF082`（`avg/sd/min/max/bad/slack/catchup/overrun/hist`）。
- 工具：`tools/fps_trace.py`（快測）、`fps_drift.py`（ppm）、
  `fps_longrun.py`（小時級）、`fps_night.sh`（一鍵）。
- 指南：`docs/02_guides/fps-stability.md`；整合報告：
  `docs/00_pm/fps-report-2026-11-25.md`。

### 10.6 相關指令

- `0xF060 MATRIX`：生產者 stats／effect／dither／color／gain／fps／zone…
- `0xF061 RENDER`：輸出層 stats（wave＋push；tests 映像）
- `0xF06B SHOWFRAME`：live frame hex（檔案驅動 show）
- `0xF079 PIXDIAG`：pixel 記憶體／配置
- `0xF077 LAYOUTBENCH`：配對＋I80 合成量測（EffectPerf）

---

## 11. 緩衝與記憶體

### 11.1 `nc_buf`：全專案唯一緩衝工具

- 一個工具：N 個等長格＋每格狀態；「在哪個記憶體」（S／P／DMA）同「是否串流」係參數。
- 三條鐵律：
  1. **開機一次要齊**：所有 buffer 開機建立，格數／格大小定死。
  2. **永不釋放**：冇 destroy；runtime buffer 例外，可用 `release()`。
  3. **執行期只切不租**：只可在預留 arena 內 carve，唔准向 heap 要。
- 代價：執行期唔可以加大 buffer；要加大就改宣告＋重開機。
- 詳細：`docs/03_notes/buffers.md`（C1–C11 契約）。

### 11.2 internal vs PSRAM（預設政策）

| 東西 | 位置 | 原因 |
|---|---|---|
| 環（i80.prd） | PSRAM（大 px） | 容量；DMA 讀取前會 msync |
| 效果值流／canvas | PSRAM | 9,600 px × 4 B／8 B |
| 6 B 配對表 | **internal only** | PSRAM 版慢過 runs（實測） |
| I80 DMA 槽 | internal（≤48 KB 時） | DMA 需要 |
| 回覆幀／parser | internal | 細、熱 |
| 任務 stack | internal | FreeRTOS |

- 不要靜默 fallback；policy 寫在 Kconfig／project defaults。
- `status sys` 的 `h_int`／`h_dma`／`h_ps` 看剩餘；`status mem` 看 buffer 帳。
- 9,600 px 的環 3 槽 ≈ 115 KB；16×600 I80 一槽 86.8 KB。
- **flat 表（6 B/px）一律 PSRAM**（`nc_px_layout_flatify()` 預設）；current＋next 兩張
  ≈115 KB。internal 只做功能緩衝（chunk 預取，Kconfig 預設關）。
- 開機 scan 入 RAM 的是 **map 定義**（共用、~30 KB），唔係每個 mode 的 flat 表。

### 11.3 常見 OOM／爆棧

- Decode handler stack 3,072 B：大 JSON／暫存用 static／heap。
- `0xF077` 一次過量 9,600 px × 4 通道會 OOM（entry 記憶體）——用 2,400 px。
- 全尺寸值流＋9,600 entry 一次過量會 OOM（≈2.4 MB > 2 MB PSRAM）——分兩段量。
- PSRAM 2 MB 的板：唔好同時開太大 canvas／packed／entry。

---

## 12. 測試與量測

### 12.1 Host 測試（唔需要板）

```bash
bash tools/hosttest/run_all.sh
```

- 21 支：`cfg_test`、`cfgstruct_test`、`ncd_test`、`task_test`、`router_test`、
  `link_test`、`mod_test`、`hub_test`、`plan_test`、`buf_test`、`portgen_test`、
  `ncm_test`、`pixel_test`、`pxEngine_test`、`pxAsset_test`、`pxProgram_test`、
  `pxLayer_test`、`pxShow_test`、`pxScan_test`、`pxLayout_test`、`fpsclock_test`。
- 全部 `-Werror`；失敗先修，唔好選擇性跑。
- ⚠ 數字要講清楚：`tools/hosttest/*/run.sh` 有 **25 支**，但 `run_all.sh` 只跑
  **21 支**測試；其餘 4 支係**量測 bench**（`gamma_bench`、`program_bench`、
  `pxMap_bench`、`sin_bench`），唔係正確性測試，所以唔入 run_all。
  講「21/21」係指 run_all；唔好講成「全部 25 支」。

### 12.2 上板 smoke test

```bash
python3 tools/nc4.py --port <PORT> status sys
python3 tools/nc4.py --port <PORT> status tasks
python3 tools/nc4.py --port <PORT> status mem
```

- `render`／`pixel_encode_task`／`pixel_paint_task`／`pixel_effect_task`：
  `level=4`、`started=1`、`running=1`。
- `status sys`：`rst` 不應該係 4（panic）；`up_s` 穩定。
- 有 tests 映像才可用 `0xF061`；有 EffectPerf 才可用 `0xF077`／`0xF065`。

### 12.3 量測原則

- 同一塊板、同一時段、同一 workload；checksum／`bus_ok` 要一致。
- 取 min／median；reps 要寫出來。
- 用物理上限做參考線（例如 16×600 線路 18 ms），唔好只看 relative。
- Host x86 數字只代表相對成本，唔可以外推 S3。
- 量測腳本：`tools/layout_bench.py`（通用）、`tools/effect_bench.py`、`tools/bench/`；
  一次性 lab：`modules/lab/i80_pack_lab/run_ab.py`。

### 12.4 已驗證基準（2026-11-24）

| 項目 | 數字 |
|---|---:|
| 9,600 px 配對（500 組，6 B 表） | 2,954 µs（舊 runs 3,745） |
| 9,600 px 完全隨機（9,598 段） | 3,007 µs（舊 11,950） |
| 每粒燈一個 group（9,600 job） | 3,009 µs（舊 19,025，6.32×） |
| 16×600 I80 合成（新） | 6,365 µs（舊 24,018，3.77×） |
| 64 px、1 lane encode | 139–165 µs（舊 357） |
| 2,400 px × 4 子幀 | apply4 0.95 ms／視覺幀；CPU 6.3 ms |

> 呢啲數字會變；以 `status.md` 最新一節為準。

---

## 13. 常見故障排查

| 症狀 | 最可能原因 | 處理 |
|---|---|---|
| `No serial data received`（esptool） | app 跑 TinyUSB，esptool 入唔到 download | 用 OTA；或 BOOT＋插電入 ROM |
| `status` 冇回覆 | port 錯／app 未起／USJ vs CDC | `ls /dev/cu.usbmodem*`；等 2 s；試另一個 port |
| port busy | 另一程式（monitor／screen／WebMaster）佔住 | 關掉；`make.py ports` 看空閒 |
| OTA `err_begin_partition_conflict` | 上次中斷，state=writing | `tools/ota.py --abort` |
| 板重啟、`rst=4` | panic（爆棧／assert／heap） | 看 log；`CONSOLE_NONE` 冇 backtrace，改用 USJ console build |
| `rst=6` | task WDT（卡住 >5 s） | 找哪個 loop 卡住；`status tasks` |
| pixel 任務 `level=-1` | `/data/config/tasks.json` 舊 | `fs_format` 重 seed（alias 已可讀舊名） |
| 板起但唔點燈 | render 未跑／腳位錯／I80 未 enable | `status tasks`、`0xF061`、hardware config |
| 換 project 後行為舊 | `/data` 仍是舊 preset | `fs_format`；確認 `status tasks` |
| 改了 sdkconfig 冇效 | build dir 的 sdkconfig 已存在 | `make.py build ... --fresh` |
| 建置 target 不符 | 殘留 `firmware/sdkconfig` | `make.py clean` 或刪舊檔 |
| 0xF077 任何 variant 都 panic | decode stack 爆（j[1200]） | 大 buffer 改 static（已修，見 §1 鐵律 8） |
| OTA 後 image 冇變 | 推錯 `.bin`（tests/release 同名） | 確認 `artifacts/<board>__<project>.bin` 時間／hash |
| `/Volumes/NC-*` 看到但寫入失敗 | FAT 由 host 掛住，device 未接管 | `nc4_file.py claim` 或 release |
| 9,600 px 量測 OOM | entry／值流太大 | 用 2,400 px 或分兩段量 |

---

## 14. AI 工作流程（照這個做）

### 14.1 開工前

> **先跑 `python3 tools/tenv.py brief`**（§0.6）：它講這台機的 IDF／板／image 同邊條
> lane 跑得。下面第 6 步的 `make.py ports` 只是其中一項事實；`blocked` lane 唔可以當跑得。

1. 讀 **`AGENTS.md`（本文件）**。
2. 讀相關計劃：`docs/00_pm/pixel-tiers-plan.md`／`roadmap.md`／`next-steps.md`。
3. 讀 `docs/00_pm/status.md` 最新一節，確認「現在點」。
4. `git status`／`git log -5`；確認工作樹乾淨、冇未 commit 的他人的改動。
5. 找出「這件事的權威文件」：協議→`commands.md`；設計→`03_notes/`；操作→`02_guides/`。
6. 有板就 `python3 make.py ports`；冇板就明講「未上板」。

### 14.2 改動時

- 先寫／改測試（host）再改實作，或至少同步加測試。
- 保持語義：zero-copy、dst 升序、後寫覆蓋、唔靜默降級。
- 改 header 後跑全套 host test（harness 會強制重建）。
- 改 production 路徑前，先確認有冇 bench／host 覆蓋。
- 大 buffer 唔好放 decode stack。

### 14.3 驗證

> 先 `python3 tools/tenv.py brief`；plan 說 `blocked`／`unknown` 的 lane **唔可以當驗證過**。
> 回報時貼 `python3 tools/tenv.py stamp`。

1. `bash tools/hosttest/run_all.sh` → 21/21。
2. `make.py build ...` → rc=0。
3. `tools/ota.py` → 推上板（唔使 BOOT）。
4. `status sys`／`status tasks`／相關 0xF0xx → 數字＋正確性閘門。
5. 冇板／未量 → 文件寫「未量」，唔好寫「應該」。

### 14.4 收尾

1. 更新計劃 checkbox 與 `docs/00_pm/status.md`（新 §7.x）。
2. 需要時更新 `03_notes/`（設計）／`commands.md`（協議）。
3. `git diff --check`；確認冇 artifact。
4. Commit（繁中 message，講為什麼＋數字）。
5. **唔好 push**，除非用家明講。

### 14.5 Definition of Done

- [ ] 功能／修正符合原 Task 的驗收條件。
- [ ] Host tests 全綠（21/21）。
- [ ] 目標 board／project 建置成功。
- [ ] 有上板驗證（或明确標示未上板）。
- [ ] 逐 byte／checksum 類正確性閘門有過。
- [ ] 計劃／status／相關 note 已更新。
- [ ] 冇未處理的 warning、冇靜默降級。
- [ ] Commit 完成（未 push）。

---

## 15. 快速命令卡

```bash
# 測試環境（每台機唔同；先睇這個）
python3 tools/tenv.py brief
python3 tools/tenv.py plan -v
python3 tools/tenv.py stamp

# 找板／建置
python3 make.py doctor
python3 make.py ports
python3 make.py projects
IDF_PATH=$HOME/esp/esp-idf python3 make.py build ESP32-S3-Matrix \
    --project ESP32-S3-Matrix-LED-Textkit

# 日常更新（OTA；唔使 BOOT）
python3 tools/ota.py --port /dev/cu.usbmodemXXXX \
    artifacts/ESP32-S3-Matrix__ESP32-S3-Matrix-LED-Textkit.bin

# OTA 失敗
python3 tools/ota.py --port /dev/cu.usbmodemXXXX --abort

# 板上狀態
python3 tools/nc4.py --port /dev/cu.usbmodemXXXX status sys
python3 tools/nc4.py --port /dev/cu.usbmodemXXXX status tasks
python3 tools/nc4.py --port /dev/cu.usbmodemXXXX status mem
python3 tools/nc4.py --port /dev/cu.usbmodemXXXX reboot

# 重 seed config（會清 /data）
python3 tools/nc4_file.py --port /dev/cu.usbmodemXXXX \
    set '{"System":{"fs_format":1}}' 1
python3 tools/nc4.py --port /dev/cu.usbmodemXXXX reboot

# Host 測試
bash tools/hosttest/run_all.sh

# 量測（EffectPerf 映像）
python3 modules/lab/i80_pack_lab/run_ab.py --port /dev/cu.usbmodemXXXX --px 9600 --variant 17 --reps 8
python3 tools/layout_bench.py --port /dev/cu.usbmodemXXXX --px 9600 --all

# Recovery（OTA 都做唔到才用）
# 拔 USB → 按住 BOOT → 插 USB → 2–3 s 後放開 → 用 ROM port
IDF_PATH=$HOME/esp/esp-idf python3 make.py flash ESP32-S3-Matrix \
    --project ESP32-S3-Matrix-LED-Textkit --port /dev/cu.usbmodem111401 --full
```

---

## 16. 文件權威順序

1. **程式碼＋實測**（`firmware/`、`tools/`、上板 log／`status`）
2. **`docs/00_pm/status.md`**（最新快照）
3. **`docs/01_protocol/`**（協議契約）
4. **`docs/02_guides/`**（操作）
5. **`docs/03_notes/`**（設計理由：現況機制）
6. **`docs/04_evolution/`**（歷史：點行到呢度；數字只作導航，現況仍以 status 為準）
7. 舊 handoff／night report（歷史；結論已併入 status）

衝突時：**程式碼＋最新量測 > status > protocol／guides > notes > evolution > 舊文件**。發現文件過期，順手修正並在
commit message 說明。

---

## 17. 這份文件的維護

- 新增／改變「鐵律」、建置／OTA 流程、板子清單、指令域 → 改這份。
- 一次只保留一套講法；舊做法直接刪，唔好留「以前係…」在正文（歷史交給 git）。
- 每次改完在下面加一行：

| 日期 | 改什麼 | 誰 |
|---|---|---|
| 2026-11-24 | 初版：專案總覽、10 條鐵律、OTA／建置／測試／pixel／troubleshooting、AI SOP | lead（AI） |
| 2026-11-24 | 版本一致性：`status fw`／OTA／image 同一個 `PROJECT_VER`；`make.py` 每次傳 `-DPROJECT_VER`；記錄 IDF v6.1.0 不存在 | lead（AI） |
| 2026-11-24 | 配置分層：`driver.json` 只放裝置參數（只有 `LED` 段會被讀）；`I80`／`SPI` 型別化入 `hardware.json`，`/data/config/hardware.json` 的 `I80` 由死段變真 | lead（AI） |
| 2026-11-25 | **鐵律 12「版本要統一」**：三個環境（macOS v6.0.3／Windows `.idf` v6.1.1／WSL v6.1.1）要一致；記錄要寫 `idf=` 同環境；附 6.0.3 vs 6.1.1 的 image 差異實測。順手把 host test 由 20 支更新為 21 支（加入 `fpsclock_test`） | lead（AI） |
| 2026-11-25 | **pixel 四層四任務正名**：`pixel_effect_task`／`pixel_paint_task`／`pixel_encode_task`／`render`；wave 獨立 component、render 係公用輸出層（唔屬 pixel）；舊 `pixel_task` 退役；`nc_task.c` 加 alias 鏈（OTA 不熄燈） | lead（AI） |
| 2026-11-25 | **`pair` → `pixel_paint_task`**：pair 唔夠直觀，改成「上色」；loop `paint`；alias `pixel_paint_task→pair→content→show`。同時把 RMT（C 系列）／PARLIO（P4）寫入 pixel-tiers-plan Task 9／next-steps §15，並新增 `pixel-ring-flow.md` 討論環的協同 | lead（AI） |
| 2026-11-25 | **`pixel_wave_task` → `pixel_encode_task`**：wave 只涵蓋 WS2812 波形，實際要出 I80／SPI／RMT（C 系列）／PARLIO（P4）多種格式；API 改 `nc_render_encode_*`；後端矩陣見 `docs/03_notes/pixel-output-backends.md` | lead（AI） |
| 2026-11-25 | **任務邊界更正**：`pixel_encode_task` 拆獨立 component（四個 component／四條任務）；`render_task` 改回公用名（唔屬 pixel 系列），encode 經 `nc_render_encode_*` 用它的環／lane／節拍 | lead（AI） |
| 2026-11-25 | **IDF 版本已統一 v6.1.1**：macOS `~/esp/esp-idf` `git describe` = v6.1.1，三個環境同版；6.0.3 的對照數字保留作歷史 | lead（AI） |
| 2026-11-25 | **數據修正（A/B 驗證後）**：P4-ETH 的 6.1.1−6.0.3 image 差由 `+30,992 B` 更正為 **`+24,688 B`**（原本係對住更舊產物）；Matrix `+9,584 B` 證實正確。另註明 25 支 `run.sh` 之中只有 21 支係測試（4 支係 bench） | lead（AI） |
| 2026-11-26 | **新增技術進化日誌 [`pixel-evolution.md`](docs/04_evolution/pixel-evolution.md)**：編碼 143 ms → 6.36 ms 的每一步（含 1.22× 失敗實驗、1-lane 回歸）、配對／環／FPS 三條主線、負結果清單、量測點警告。§3 目錄地圖、找東西次序加第 6 條、§10.4 加指針 | lead（AI） |
| 2026-11-26 | **新增測試環境層 [`tenv`](docs/02_guides/test-environment.md)**：`tools/tenv.py`（init／bind／brief／plan／stamp／verify／selftest）＋`tools/testenv/policy.json`（IDF 要求、host test 支數）＋`.testenv/`（本機意圖／事實，唔進 git）。§0.6 新章、§14.1／§14.3 開工先跑、§15 命令卡；同時修 `ncbuild/env.py` 的空 `.idf` 遮住真 IDF、Windows COM 列舉 | lead（AI） |
| 2026-11-26 | **再加全樹版 [`tech-evolution.md`](docs/04_evolution/tech-evolution.md)**：9 條主線（NC4 收發／通道／緩衝／任務棧／效果層／配對／編碼／USB-FAT／建置）＋§11 跨主線負結果（12 條）＋§12 量測規矩；pixel 專文保留做細節。找東西第 6 條改成「全樹→專文」兩層 | lead（AI） |
| 2026-11-26 | **進化日誌獨立成層 `docs/04_evolution/`**：`tech-evolution.md`＋`pixel-evolution.md` 由 `03_notes` 搬入（`git mv`，歷史保留），新增 [`04_evolution/README.md`](docs/04_evolution/README.md)（判準／導航／維護規則／新主線骨架）。§3 目錄地圖、找東西第 6 條、§10.4、§16 權威順序、§17 一齊改 | lead（AI） |
| 2026-11-26 | **I80 兩個實作（esp_lcd／直驅）＋設計紅線**：直驅只 route 真正接燈的 lane（Matrix 由 10 支腳變 1 支，50 fps／drops=0），但**不得異化成只for燈**——cfg 保留 `wr_pin`／`dc_pin`／`cs_pin`，真熒幕仍走 esp_lcd；直驅的 cmd phase／多 device 未做且明講（見 pixel-i80-pin-schema.md §8.4、status §7.254） | lead（AI） |
| 2026-11-26 | **我們自己的 I80 驅動完整 LCD 化**：`nc_i80_drv_submit_cmd()`（cmd phase＋param phase）、`cmd_bits`、DC 四段電平、CS assert／de-assert；`wr`／`dc`／`cs` **全部可選**（唔填 -1 唔影響 LED），唔會像 esp_lcd 缺一就整條 lane 唔建立。LED 快路徑加 phase 形狀快取：enc_us 209→186、tx_us 38→32，50 fps／drops=0。面板未上板（冇硬件）。status §7.255 | lead（AI） |
| 2026-11-26 | **Windows 主機的 bash 陷阱**：`make.py`／`doctor` 用 `bash -c` 傳 `C:\…` 路徑，Git Bash 同 WSL bash 都會失敗（doctor 的 board check 會假報 FAIL）。§0.5 新增一節講點判斷、建置改在 WSL 內跑（`bash -lc 'cd /mnt/c/… && IDF_PATH=/home/user/esp/esp-idf python3 make.py …'`）、OTA／NC4 留在 Windows 側。同時記錄 Matrix-LED-Textkit 全面 JSON 化（見 status §7.253） | lead（AI） |

---

> **最後一句**：呢個 repo 的默認狀態係「有計劃、有量測、有文件」。
> 你加的每一行 code，最後都要能夠回答三個問題：
> **為什麼這樣做？點證明它work？邊份文件記錄了？**
