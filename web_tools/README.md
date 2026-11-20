# ESP WebMaster（網頁工具，全部喺呢個資料夾）

獨立嘅 ESP 專用 NC4 工作臺（唔依賴 mp_Net-Core）。UI 跟 `mp_Tools` 的 workbench 版面：
左邊 activity rail、頂部 device strip、左側裝置面板、底部終端／活動、status bar。

**核心設計：一個 server 可以同時連多台裝置；每台由 server 獨佔自己嗰條序列埠，
Web UI 同 AI 都經同一個 API 讀同一份 log／activity。**
所以唔會再出現「你連咗我就連唔到」或者「兩邊各自開 COM port 搶線」；
亦唔需要為第二塊板再開一個 server。UI 用 header 的 device selector 切換；
AI 用 `POST /api/select` 切 active，讀指定裝置 log 再加 `device=`。
人用瀏覽器、AI 用 REST（帶 `X-Actor: ai`）。

## 呢個資料夾有咩

網頁工具**全部收喺 `web_tools/`**；repo 根只留建置工具（`make.py`／`build.sh`／`make.bat`）。

```text
web_tools/
├─ README.md               ← 你在這裡（唯一入口文件）
├─ settings.json           ★ 基礎數值（port／host／預設連邊個裝置／esptool／UI 首頁）
├─ settings.local.json     ★ 只係我部機的覆寫（選用，gitignore）
├─ WebMaster.bat           Windows 雙擊開（→ web.ps1）
├─ web.ps1                 Windows 完整啟動器
├─ web.sh                  macOS／Linux 一鍵開
├─ run_web.sh              Linux 通用入口（用絕對路徑叫 web.sh）
├─ WebMaster.command       macOS 雙擊開
└─ webmaster/              ← server ＋ 前端（唔應該直接雙擊呢度）
   ├─ settings.py          讀設定／驗證／合併（＋內建預設池）
   ├─ run.py  launch.py    起 server／自動建 venv
   ├─ server.py            REST／WS
   ├─ scripts.py           啟動腳本清單（「啟動」頁的來源）
   ├─ formatting.py        JSON 格式化（server 係唯一真相）
   ├─ container.py         .nct／.nce／.nca 容器解析
   ├─ files.py link.py nc4.py sim.py flash.py ota.py recover.py events.py
   ├─ env.py               環境自檢＋安裝（doctor／IDF）
   ├─ console.py           Windows 控制台編碼保護（cp950）
   ├─ static/
   │  ├─ index.html        外殼＋樣式（單頁）
   │  ├─ style.css
   │  └─ js/               14 個模組（見下面「前端結構」，零 build step）
   ├─ tests/               pytest（108 passed ＋ 1 skipped）
   ├─ requirements.txt     執行期依賴
   └─ .simfs/  .venv/      ← 生成物（gitignore）
```

> **設定檔為咩放喺 `web_tools/` 而唔係 `webmaster/`**：改 port／預設裝置係
> 「打開工具」那一層的事，你打開 `web_tools/` 就應該見到，唔需要入程式目錄搵。
> 程式入面只有**預設池**（`settings.py` 的 `DEFAULTS`），冇填的 key 就落返預設。

## 設定檔（基礎數值）

所有「數字／開關」集中喺 **`web_tools/settings.json`**（同啟動器同層），
唔再散落喺 shell、PowerShell、Python 三處各有各預設（以前改咗設定但仍然報
8010 被佔用，就係咁嚟）。

**可以只寫你想改的 key**——其餘自動落返程式的預設池（深層合併）：

```jsonc
// web_tools/settings.json（節錄；完整的檔有註解）
{
  "server": {
    "host": "127.0.0.1",
    "port": 8010,          // HTTP port
    "auto_port": true,     // ★ port 被佔用 → 自動揀下一個（唔會卡住啟動）
    "auto_port_scan": 20,  //   最多試幾個
    "open_browser": true,  //   啟動後開瀏覽器
    "log_level": "info"
  },
  "device": {
    // "auto" = 自動連插住嘅真板（搵唔到就唔連，唔會偷偷開模擬）
    // "sim" / "COM13" / "/dev/cu.usbmodem…" / null（開 UI 自己揀）
    "autoconnect": "auto",
    "prefer_port": null,   // auto 時優先揀呢個埠（例如 "COM20"）
    "baud": 115200,
    "sim_dir": null
  },
  "flash": {               // 固件頁 ESPTOOL 卡的預設值
    "esptool_chip": "",    // 空 = 自動判斷
    "esptool_baud": 460800,
    "esptool_before": "usb_reset",
    "esptool_offset": "0x0"
  },
  "ui": { "default_view": "launch" }   // 冇 hash 時開邊一頁
}
```

只改 port 就寫咁多就夠：

```jsonc
{ "server": { "port": 8020 } }   // 其餘全部照預設池
```

**優先次序：命令列 > `settings.local.json` > `settings.json` > 程式內建預設池。**

| 想做 | 點做 |
|---|---|
| 改 port | 改 `server.port`（或者 `--port 8011`；命令列永遠贏） |
| 唔想自動換 port | `"auto_port": false` → 被佔用會直接報錯並提示換埠 |
| 開機就連真板 | 預設就係 `"autoconnect": "auto"`：自動揀插住的 ESP（VID 白名單），冇板就唔連 |
| 固定一個埠 | `"autoconnect": "COM20"`，或者 `"prefer_port": "COM20"` |
| 想用模擬 | `"autoconnect": "sim"`（或者啟動時 `--sim`） |
| 只係我部機唔同 | 開 `web_tools/settings.local.json` 寫覆寫（**唔入 git**） |
| 睇而家真正生效 | `python run.py --print-config` 或 UI「啟動」頁的**目前設定**卡 |
| 重設成預設 | `python run.py --write-config --force` |

**壞值要大聲**：打錯 key 名／型別錯／port 超出範圍 → 直接報錯並指出係邊個 key 同邊個檔
（`server.port` 收到 `"8010"` 會即刻講，唔會靜靜用 8010）。未知的 key 只警告
（一路容忍 `_comment` 之類）。環境變數 `ESPWM_PORT`／`ESPWM_HOST`／
`ESPWM_AUTOCONNECT`／`ESPWM_LOG_LEVEL` 亦可以覆寫。

## 啟動（最簡單）

**先揀你部機嘅平台**——同一件事每個平台都有一個專屬檔，唔會再用錯。

| 平台 | 開網頁工作臺（`web_tools/` 內） | 建置／燒錄（repo 根） |
|---|---|---|
| **Windows** | 雙擊 **`WebMaster.bat`**（或 `web.ps1`） | `make.bat doctor`／`make.bat build s3` |
| **macOS** | 雙擊 **`WebMaster.command`**（或 `./web.sh`） | `./build.sh doctor`／`./build.sh build s3` |
| **Linux** | `./web.sh`（或 `./run_web.sh`） | `./build.sh …` |

```powershell
# 由 repo 根跑（Windows）：雙擊 web_tools\WebMaster.bat 就係最快路徑
.\web_tools\web.ps1                         # 模擬裝置（免硬件）＋開瀏覽器
.\web_tools\web.ps1 -Serial COM13           # 真板（S3 原生 USB CDC）
.\web_tools\web.ps1 -Serial COM13 -Port 8011 -AutoPort
.\web_tools\web.ps1 -Fresh                  # venv 壞咗：刪掉重建
```

```bash
# 由 repo 根跑（macOS／Linux）
./web_tools/web.sh                 # 自動建 venv＋裝 deps＋開瀏覽器（預設 port 8010）
./web_tools/web.sh --sim           # 用模擬裝置，唔使板
./web_tools/web.sh --serial /dev/cu.usbmodemXXXX --port 8011
```

- **`WebMaster.bat` 唔會要你改 ExecutionPolicy**：佢只揀一個 Python（`py -3` → `python` → `python3`）
  再交棒俾 `web.ps1`（`-ExecutionPolicy Bypass` 只作用於該次 Process）。
  `.bat` 刻意保持純 ASCII——cmd.exe 用主控台代碼頁解 `.bat`，UTF-8 中文會變亂碼甚至爆語法；
  中文訊息全部由 `web.ps1`（UTF-8 + BOM）輸出。
- **launcher 唔再自己寫死 port／`--open`**：佢只把你明確俾的參數轉交 `run.py`，
  其餘全部跟 `settings.json`。所以改完設定檔，用返同一個雙擊檔就會生效。
  （`web.ps1` 會先用 `run.py --print-config` 印一行「設定檔：port=… autoconnect=…」，
  方便一眼確認讀到邊個檔。）
- Windows 第一次跑會建 `web_tools\webmaster\.venv` 並 `pip install -r requirements.txt`
  （用 `--no-cache-dir`：本機 pip 快取目錄可能被 ACL 鎖住，寫 wheel 快取會令整個 install 失敗）。
- 手動（任何平台，已有 deps）：

```bash
cd web_tools/webmaster
python run.py --sim
python run.py --serial COM13 --port 8011 --open
python run.py --print-config        # 睇實際設定（唔會起 server）
```

瀏覽器開 `http://127.0.0.1:8010/`（或者 `--print-config` 見到的那個 port）。

## 真板優先（唔會偷偷開模擬）

`device.autoconnect` 預設 **`"auto"`**。啟動時會**掃一次**序列埠，然後揀一塊板連上去。

### 掃描做咩、唔做咩

| | 內容 |
|---|---|
| **會** | 讀 `serial.tools.list_ports.comports()` 的**中繼資料**（埠名、description、VID／PID、序列號）。Windows 上是問 SetupAPI／registry，**幾十毫秒**、一次性。 |
| **唔會** | **唔會逐個埠開一次試**。開埠會打斷板上正在跑的程式，也可能搶走別的程式（Arduino 監控／PuTTY）正在用的連線。真正開埠只會開最後揀中的那一個。 |
| **判斷** | 只認有 VID 的 USB 序列，白名單 `0x303A`(Espressif 原生 USB)／`0x10C4`(CP210x)／`0x1A86`(CH34x)／`0x0403`(FTDI)／`0x1B4F`／`0x2341`；**排除** Intel AMT SOL、藍牙 SPP（有 VID 都排除）、冇 VID 的虛擬埠。 |
| **排序** | Espressif 原生 USB 優先，其後按埠名。想指定就用 `device.prefer_port` 或 `--serial`。 |
| **揀唔到** | **唔連**（唔會 fallback 模擬）；log 會列出每個被跳過的原因。 |

### 啟動時你會見到

```text
[esp_webmaster] 掃描序列埠：共 4 個 -> 疑似板 1 個（只讀 USB 中繼資料，唔會逐個開埠試）
[esp_webmaster]    選中 COM20（Espressif 303A:1001）
[esp_webmaster]    跳過：COM3=Intel AMT SOL、COM32=藍牙序列埠、COM33=藍牙序列埠
```

插了兩塊板時會多一行「另一個板 COM21（CP210x）—— 要指定就 --serial COM21」。
冇板時只有掃描行＋「（冇疑似板：確認 USB 線插好、板有電、驅動正常）」。

想**只看偵測結果、唔起 server**：

```bash
python run.py --list-ports
# 或
curl -s http://127.0.0.1:8011/api/ports | python -m json.tool
# → data[].board=true / suggested="COM20"
```

UI 用同一套判斷（`/api/ports` 的 `board`／`suggested`）：真板有 **★**、排最前、
下拉預設選中佢；「模擬」按鈕寫明 **模擬（無硬件）**，裝置下拉顯示「模擬裝置（無硬件）」
唔會再同真板撈亂。

## 頁面

| 頁 | 內容 |
|---|---|
| ⚙ 設備 | sys / storage / log；接管／交還 FAT、重建 FAT、除錯 log、重啟 |
| **⌨ IDE** | **檔案頁已併入這裡**：樹狀總管＋多 tab＋CodeMirror（JSON/C/Python/XML/Markdown/Shell）＋JSON lint／格式化／主題；開檔／新檔／拖拉上載／下載／刪除／確認／復原／FILE_SCAN |
| 💾 隨身碟 | 可選（有 USB 的板）：交還 FAT → 揀電腦上嘅碟 → 直接拖拉／編輯 → 接管 FAT |
| 🔥 固件 | **三條燒錄路徑**：① NC4 OTA ② server 側 ESPTOOL（ROM／USJ）③ **頁面燒錄（esptool-js／Web Serial）**；晶片探測、offset／baud／erase／flashMode／flashFreq／flashSize |
| ⌨ 指令 | 任意 NC4 cmd + hex payload + expect；底部 `>>>` 快速指令 |
| ⚙ **環境** | **doctor＋一鍵補齊**：開機自動檢查一次；列出 11 項工具（缺咗有咩後果、點補）、5 種「能力」（開 server／NC4 OTA／server esptool／頁面燒錄／建置 firmware）；可一鍵跑官方安裝（`deps`／`esptool`／`ESP-IDF`），輸出即時串流。 |
| 📦 容器 | 在 IDE 檔案樹打開 `.nct`／`.nce`／`.nca` 會顯示**容器檢視器**（唔開二進位編輯器）：檔頭／manifest（kind、target、core、layer、prio、fps、px、mem、stack、budget）／meta（name、desc、usage、requires、provides）／宣告指令／payload CRC／原始位元組。 |

## 前端結構（`static/js/`，零 build step）

UI 拆成 15 個模組，用傳統 `<script>` 按次序載入（唔用 ES module、唔需要打包）。
**載入次序＝檔案名數字**：`00-core` 一定第一，`99-app` 最後。

```text
static/
├─ index.html          外殼＋內嵌樣式（單頁）
├─ style.css
└─ js/
   ├─ 00-core.js        state、$／$$、fmt*、toast、api（fetch 包裝）
   ├─ 10-connection.js  序列埠、裝置切換、WebSocket、handleEvent
   ├─ 20-terminal.js    logLine（終端）、activityLine（AI 操作）、showPanelTab
   ├─ 30-status.js      refreshStatus：sys／storage／log 三張卡
   ├─ 40-files.js       manifest、上傳／下載／刪除（CDC FAT）
   ├─ 50-drive.js       隨身碟（MSC 捷徑）
   ├─ 60-flash.js       NC4 OTA ＋ server 側 ESPTOOL
   ├─ 65-browser-flash.js 頁面燒錄（esptool-js／Web Serial）＋序列埠交接
   ├─ 66-flash-request.js  AI 請求燒錄：橫幅、claim、執行、回報
   ├─ 70-console.js     NC4 console、快速指令、parseStatusAck／statusSet
   ├─ 75-shell.js       switchView（rail）＋ bindUI（所有按鈕）
   ├─ 80-ide.js         IDE：檔案樹、多 tab、CodeMirror、lint、格式化
   ├─ 85-container.js   NCM 容器檢視器（.nct／.nce／.nca）
   ├─ 90-env.js         環境頁（doctor＋安裝進度）
   └─ 99-app.js         init、deep link、鍵盤快捷鍵
```

**分工原則**：瀏覽器 JS 只做畫面；**碰作業系統／硬體的事一律在 server（Python）**——
序列埠、檔案系統、`settings.json`、esptool。所以同一份 UI 在 Windows／macOS／Linux
完全一樣，平台差異只出現在 `link.py`／`flash.py`（`serial.tools.list_ports` 已處理）。

- JS 不自己判斷「這個埠是不是板子」：`GET /api/ports` 回 `board`／`suggested`，
  前端只負責排序與顯示（避免兩套 heuristic 打架）。
- `tests/test_frontend.py` 會檢查：模組清單與載入次序、每個檔有 `"use strict"`、
  `node --check` 語法、**JS 用到的 `$("#id")` 全部存在於 index.html**（漏 id 是拆檔
  後最容易中的錯）。
- 鍵盤：`1`–`5` = 設備／IDE／隨身碟／固件／指令。

## 啟動腳本 API（無 UI）

「啟動腳本」頁面已移除（你已經打開了工具，站內再教一次啟動是多餘的）。
但 `GET /api/scripts`／`GET /api/scripts/one?rel=…` 仍然保留，給 AI／CLI 用：

```bash
curl -s http://127.0.0.1:8011/api/scripts | python -m json.tool | head -30
# → 分類（webmaster/build/device/ai）× 平台 × 摘要 × 指令
```

## 波形模擬器（六工具合成，`#wave`）

左邊 rail「🌊 波形」＝離線波形預覽：讀 `program`（`keep`／`math_now`／`square_wave_now`／
`pulse_wave`／`pulse`／`starter`），用**同 firmware 一樣嘅公式**（`nc_px_engine_program`）
即時算波表，畫整條燈鏈＋時間曲線＋段組合，50 fps 播放。**唔使連板**。

| 入口 | 用法 |
|---|---|
| 貼 JSON | effects.json／單一效果／純 program 陣列 → 「解析／預覽」 |
| 讀取裝置檔案 | 填路徑（例如 `/pixel/effects/effects.json`）→ 經 `/api/file` |
| 列出裝置效果 | 由 manifest 篩 `/pixel/effects/*.json` → 下拉即載（要連線） |
| 開啟本機檔 | 直接揀 `.json` |
| IDE | 開檔後按工具列「🌊 波形」→ **由裝置路徑直接讀同一個檔**（唔使貼），URL 變成 `#wave?file=…`，refresh 都仲喺同一條效果 |

- **段編輯**：表格即場改 `type／end_Time／l_max／l_lim／F／phi`，畫面即時更新；
  「＋段」「✕」增刪；「複製 JSON」帶走。
- **存到裝置**：`PUT /api/file?path=…`（預設 `/pixel/effects/effects.json`），
  會保留原檔的 `version`／其他效果，只換被編輯嗰條。
- deep link：`#wave`（空白）或 `#wave?file=/pixel/effects/effects.json`（自動讀裝置檔）。
- `static/index.html` 的 js／css 有 `?v=` 版本戳，改前端之後唔會食到舊 cache。

## 容器檢視（`.nct` / `.nce` / `.nca`）

IDE 左邊檔案樹揀 `.nct`／`.nce`（或任何帶 NCM magic 的 `.nca`）→ 右邊會出容器摘要：

- **檔頭**：magic／ABI／flags（AOT／MANIFEST）／payload offset+size+CRC／target／manifest offset；
- **manifest**：kind（task／effect）、core／layer／prio、fps／px、mem／stack／budget、宣告指令（外掛私有 cmd）；
- **meta**：name／desc／usage、`requires`（type／name／bytes／exclusive）、`provides`；
- **驗證**：magic／版本／payload CRC／區塊界線；有問題會逐條列出（仍然會顯示已讀到的部分）；
- `.nca` 等未知副檔名會照樣解析，但會提示「裝置只掃 `.nct`／`.nce`，唔會載入」。

API：`GET /api/container?path=/tasks/xxx.nct`（server-side 讀 FAT，唔會把整份檔落瀏覽器）。
解析器在 `container.py`（純 Python，同 `tools/ncm.py`／`nc_ncd.h` 對齊）。

## 共享 log／AI API（重點）

序列埠由 server 擁有；AI **唔會**再開第二條 COM port。所有操作經 REST：

```bash
# 看最近 100 行（同一份 ring：line／frame／activity）
curl -s 'http://127.0.0.1:8010/api/log/tail?n=100'

# AI 讀狀態（活動面板會見到 [ai] GET /api/status）
curl -s -H 'X-Actor: ai' 'http://127.0.0.1:8010/api/status?keys=sys'

# AI 改檔（server 會自動 confirm；活動面板見到 [ai] PUT /api/file）
curl -s -X PUT -H 'X-Actor: ai' --data-binary @config.json \
  'http://127.0.0.1:8010/api/file?path=/config/config.json'

# AI 留一句話（純活動記錄）
curl -s -X POST -H 'X-Actor: ai' -H 'Content-Type: application/json' \
  -d '{"text":"檢查 /config/system.json"}' 'http://127.0.0.1:8010/api/log/note'

# AI 能力清單
curl -s 'http://127.0.0.1:8010/api/ai'
```

- 讀取只會回最近 ring（預設 4,000 個事件）；`since=<seq>` 可以增量拿。
- 多裝置：`GET /api/devices`、`POST /api/select {"device":"..."}`；
  讀指定裝置 log 用 `GET /api/log/tail?device=/dev/cu.usbmodem1401&n=200`。
- UI 會跟隨 server 的 active device（AI 切換時 UI 會自動切）；如果 IDE 有未儲存檔案，
  會提示並切回原裝置，避免覆蓋。
- WebSocket `/ws/log` 連上會先收到 `backlog`，之後即時；事件帶 `device`，
  UI 只顯示 active 裝置；多個瀏覽器可以同時看。
- 所有 mutating API 同 `X-Actor: ai` 的讀取都會寫入 `activity`；UI 底部「活動」分頁即時顯示。
- UI 右上角會顯示誰在做什麼（actor）；AI 的操作唔會再隱形。
- 檔案寫入會廣播 `file_changed`：IDE 有開同一個檔會自動 reload；如果本地有未儲存修改，
  會提示「AI 改了這個檔，儲存會覆蓋」——兩邊同時編輯不會靜默蓋掉。

### Deep link

```
http://127.0.0.1:8010/#launch                                   # 啟動腳本頁（入門頁）
http://127.0.0.1:8010/?file=/config/system.json#ide             # IDE 直接開檔
http://127.0.0.1:8010/?panel=activity#ide                       # 預設顯示活動分頁
```

`#launch` 啟動頁；`#ide` 開 IDE；`file=` 自動開檔；`panel=activity` 預設顯示活動分頁。

> 冇 hash 時預設開 **`#launch`**（入門頁）；如果連上裝置就自動跳去 `#device`。
> 鍵盤 `1`–`6` 依次係 啟動／設備／IDE／隨身碟／固件／指令。

## IDE

- 左邊樹由 `/manifest.json` 砌；`⟳` = `FILE_SCAN(0x200B)`；檔案有 **JSON／C／PY／MD／JS／BIN 顏色分類**。
- 多 tab（每 tab 有類型色點）、未儲存圓點、`Ctrl/Cmd-S` 儲存、`確認`／`復原`（.bak）、下載、**刪除**。
- **JSON lint**：即時標紅、gutter 標記、`JSON ✓ / ✗`；**格式化**（標準 2 空格）與 `Ctrl/Cmd-Shift-F`。
  格式化一律經 `POST /api/format`（`formatting.py`，server 係唯一真相）：壞 JSON **唔會改內容**，
  只回報「第幾行、第幾欄」；副檔名唔係 `.json` 或檔案 >1 MiB 直接拒絕。
- **主題**：Bench／Material／Dracula／Nord／Solarized（離線 CSS；記在 localStorage）。
- **拖拉上載**到整個 IDE；上載／儲存都走 CDC FAT `BEGIN → CHUNK → END`，server END 後自動 `FILE_CONFIRM`。
- CodeMirror 5、lint addon、主題全部 vendor 在 `static/vendor/codemirror/`（離線可用，含 LICENSE）。
- 二進位檔會自動改為下載。

## 燒錄：三條路徑（揀對那條）

| | ① NC4 OTA | ② server esptool | ③ 頁面燒錄（esptool-js） |
|---|---|---|---|
| API | `POST /api/firmware` | `POST /api/flash/esptool` | `POST /api/port/release` → 瀏覽器 → `/api/port/reclaim` |
| 協議 | `0x2210` caps → `0x2201` BEGIN → `0x2202`×N → `0x2203` END → `0x2220` APPLY | esptool ROM（`--no-stub`） | esptool-js（Web Serial） |
| 要放開序列埠 | **唔需要** | 需要（server 自動做） | **需要**（手動交接） |
| 要 download mode | 唔需要 | 需要（或 USJ） | 需要（或 USJ） |
| 速度 | 實測 194–213 KB/s | P4 ~1 MB/17 s | 視 USB／baud |
| AI／CLI 可用 | ✅ | ✅ | ✅ **可發起**（`POST /api/flash/request`；最後一下要人按） |
| 適用 | S3／C3 日常更新 | P4、救磚、首燒 bootloader | 零安裝、Chrome／Edge |

**建議**：S3 日常用 ①；P4／救磚用 ②；想純瀏覽器不裝 Python 用 ③。

### 頁面燒錄的序列埠交接（③ 的關鍵）

COM 埠同一時間只有一個主人。所以 UI 是「三步」而不是一個按鈕：

```
1 · 交出序列埠   POST /api/port/release   → server 關 link，記住 State.handover
2 · 揀埠並燒錄   navigator.serial.requestPort() → ESPLoader.main() → writeFlash()
3 · 交還序列埠   POST /api/port/reclaim   → server 重新連線（自動）
```

- 交接期間 server **唔會**偷偷重開埠（否則就是搶埠）；其他 API 會回
  `409 序列埠已交畀瀏覽器（頁面燒錄中）`，唔會靜靜 timeout。

### AI 也可以發起頁面燒錄（人在最後按一下）

**為什麼不能全自動**：Web Serial 的 `navigator.serial.requestPort()` 必須由
**user gesture** 觸發——這是瀏覽器安全模型，任何網站都繞不過，不是我們的限制。
所以做成「AI 下指令 → 人按一下 → 結果回 AI」：

```bash
# 1) AI 排隊（上傳 .bin bytes）
curl -s -X POST 'http://127.0.0.1:8011/api/flash/request?offset=0x0&mode=dio&freq=40m&size=4MB' \
     -H 'X-Actor: ai' --data-binary @app.bin
# → {"data":{"id":"1a1e…","status":"pending", …}}

# 2) UI 會自動彈出「AI 請求燒錄」橫幅（WS 事件 flash_request）；
#    人按「執行」→ 自動 release 埠 → 選埠 → 燒 → reclaim → 回報

# 3) AI 事後查結果
curl -s http://127.0.0.1:8011/api/flash/request
# → {"data": null, "history": [{"id":"1a1e…","status":"done"}]}
```

| API | 用途 |
|---|---|
| `GET /api/flash/paths` | 三條路徑邊條 AI 用得到、要唔要交接、`humanGestureRequired` |
| `POST /api/flash/request` | AI 排隊；**同一時間只准一個 pending**；上限 8 MiB |
| `GET /api/flash/request` | 目前 pending ＋ 最近 5 條歷史（AI 讀結果） |
| `GET /api/flash/request/{id}/image` | 瀏覽器下載映像去燒 |
| `POST .../claim` / `.../result` / `.../cancel` | UI 接手／回報（ok,error）／拒絕 |

安全與整潔：映像存 `webmaster/.flashq/`（gitignore），**燒完或取消即刪**；
請求帶 `by`（來源）、`note`、`offset`、mode／freq／size／eraseAll。

- `GET /api/port/state` 回 `{handover, port, ageSec, stale, browserFlash}`；
  `stale` = 超過 10 分鐘冇交還（頁面可能關了），可以手動 reclaim。
- 設 `flash.browser_flash=false` 可以整張卡停用。
- 瀏覽器只會列出 ESP 相關 VID（303A／10C4／1A86／0403）；**Safari／Firefox 沒有 Web Serial**。

### esptool-js 是 vendored 的

`static/vendor/esptool-js/`（308.5 KB，Apache-2.0，含 `LICENSE.txt` 與 `VENDOR.md`）。
`bundle.js` 是 **ES module**，所以多一層 `loader.js` 掛到 `window.esptooljs`
（本專案其他 JS 都是傳統 script，不用打包）。想改內部行為請改上游再重新 vendor；
本專案在 `65-browser-flash.js` 外面包一層（進度、錯誤、交接、自檢）。

## ESPTOOL 燒錄

- 頁面「固件」第 3 卡：offset／baud／chip／before／erase／揀 `.bin`。
- server 會**先放開序列埠**，跑 `esptool`（ROM／USJ），完成後自動重新連線；UI 同 AI 都不用自己搶 port。
- 「探測晶片」= `esptool --no-stub flash_id`（只讀，安全）。
- **S3 的 TinyUSB CDC 請用上面的 NC4 OTA**；ESPTOOL 適用於 P4 USJ、ROM download mode、或已經入 bootloader 的板。
- 已實測（P4-C6，`/dev/cu.usbmodem1401`）：探測回 `ESP32-P4`、16 MB flash；
  `offset=0x20000` 寫入 P4 app（1.0 MB）rc=0、約 17 s，完成後自動重連、STATUS 正常。
- esptool 寫入用 `--no-stub`（ROM）；P4 USJ 的 stub 上載會爛（`Invalid head of packet`），
  所以 server 固定走 ROM 路徑，速度慢一點但穩定。
- 為什麼不是瀏覽器的 Web Serial（esptool-js）：Web Serial 會同共享 broker 搶同一個 port，
  這樣就違背「UI／AI 同時用」；server-side esptool 用同一個 ROM 協議，功能等同但唔會搶線。

## CDC 檔案流程

1. USB 插住板，UI 連 CDC。
2. 「檔案」頁 → **重建索引**（`0x200B FILE_SCAN`）→ 讀 `/manifest.json` 砌樹。
3. 拖拉上傳：`BEGIN → CHUNK×N → END`（每包 4 KB，等 ACK）。
4. 覆蓋舊檔時 device 先留 `<path>.bak`；server 喺 END 成功後自動送
   `0x2008 FILE_CONFIRM`，所以正常唔會見到「待確認」。
5. 下載／編輯：`FILE_QUERY` → `FILE_READ` → 文字檔可直接改；儲存經同一 PUT，亦會自動 confirm。
6. 寫入需要裝置擁有 FAT；如果 `storage.mode=usb`，先按 **接管 FAT**
   （`0x1103 {"System":{"file_mode":1}}`）。

協議細節見 [`docs/03_notes/file-transfer.md`](../../docs/03_notes/file-transfer.md)；
上板驗收見 [`docs/02_guides/esp-webmaster-test.md`](../../docs/02_guides/esp-webmaster-test.md)。

## 協議

| 指令 | 用途 |
|---|---|
| `0x1101` STATUS_GET | sys / storage / log |
| `0x1103` STATUS_SET | `System.file_mode`（0=交還、1=接管）、`fs_format`、`log_debug_mode` |
| `0x100F` REBOOT | 重啟 |
| `0x2001–0x2004` | 上傳（BEGIN/CHUNK/END/ACK） |
| `0x2005` FILE_QUERY | 單檔 metadata |
| `0x2007` FILE_READ | 下載（回 `0x2002`） |
| `0x2008/0x2009/0x200A` | CONFIRM / DELETE / UNDO |
| `0x200B` FILE_SCAN | 重建 `/manifest.json` |
| `0x22xx` OTA | 固件更新（`0x2210` caps；`0x2201/0x2202/0x2203/0x2220`） |

所有 device 操作經一把 lock 序列化；log 行（`0x1E`）同 NC4 幀由同一個嚴格分幀器分流。

## 環境自檢與安裝（`env.py`／`/api/env*`）

開機時自動檢查一次（只讀，不寫任何東西），缺的項目會印出來並指向「環境」頁。

```bash
curl -s http://127.0.0.1:8011/api/env | python -m json.tool | head -40
```

回傳要點：

- `tools[]`：11 項（python／pip／deps／esptool／git／cmake／ninja／node／
  **idf**／**idf_toolchain**／serial_driver），每項有 `status`（ok／warn／missing）、
  `version`、`impact`（缺咗有咩後果）、`fix`（點補）。
- `ready{}`：**五種能力分開講**，因為「燒得到」不等於「建置得到」：
  `run_server`／`flash_nc4_ota`／`flash_esptool`／`flash_browser`／`build_firmware`。
  例如沒有 ESP-IDF 時 `build_firmware=false`，但三種燒錄仍然 `true`。
- `installDir`：IDF 預設安裝位置（`~/esp/esp-idf`，可用 `ESPWM_IDF_DIR` 覆寫）。

安裝（**一定要 `confirm: true`**，一次只准一個工作，輸出即時可讀）：

```bash
# 只裝 Web deps（幾十秒）
curl -s -X POST http://127.0.0.1:8011/api/env/install   -H 'Content-Type: application/json'   -d '{"what":"deps","confirm":true}'

# 裝 / 補齊 esptool
curl -s -X POST .../api/env/install -d '{"what":"esptool","confirm":true}'

# 裝 ESP-IDF（clone 官方 repo + install 腳本，10–30 分鐘）
curl -s -X POST .../api/env/install   -d '{"what":"idf","target":"esp32s3","confirm":true}'

# 追進度（since = 已讀行數）
curl -s 'http://127.0.0.1:8011/api/env/install?since=0'
```

#### 版本新鮮度：唔止「有冇裝」，仲要講「係唔係最新」

之前只顯示 `esptool OK v5.6.0`，用戶會以為「冇幫我更新」。現在會問 PyPI
（快取 6 小時，離線不報錯）並顯示：

| 顯示 | 意思 |
|---|---|
| `已是最新` | 已安裝 == PyPI 最新 |
| `5.3.1 → 5.6.0（可更新）` | 有新版本；行尾有「更新」按鈕 |
| `未知（離線？）` | 問唔到 PyPI，唔會當失敗 |

- 監看：`esptool`／`fastapi`／`uvicorn`／`pyserial`。
- `esptool` 的安裝命令已改成 `pip install --upgrade esptool`（**更新**，唔止安裝）。
- `auto_install` 為 `safe`／`all` 時，**esptool 有新版本也會自動更新**。

#### 開機自動安裝「做了什麼／為何冇做」——UI 睇得到

`/api/env` 回 `auto: {mode, plan, last}`，UI「環境」頁最上面一張卡會顯示：

```
開機自動安裝                        [all（含 ESP-IDF）]
  ❌ 唔可以自動補 idf：建置 firmware 唔可用，但冇可寫的安裝位置
  　→ 所有預設位置都唔可寫：… 請喺 settings.json 設 "env": {"idf_dir": "D:/esp/esp-idf"}
  套件版本：全部已是最新
```

- `plan` 就係「按你嘅模式，開機時會做咩」；`blocked:true` 會帶 `hint`。
- `last` 係上次自動安裝結果（成功／失敗＋時間）。
- 工具表每一行都有動作按鈕：缺的→「安裝」、可更新的→「更新」，
  一按就帶你去安裝卡並預選項目。

#### UI 一鍵設定（寫 `settings.local.json`，唔會改團隊的 `settings.json`）

- 「用可寫位置」：由 server **實測**每個候選的寫入權限，填第一個真正可寫的。
- 「儲存到本機設定」：把 `idf_dir`／`idf_version`／`idf_targets` 寫入
  `web_tools/settings.local.json`（API：`POST /api/env/config`），重開即生效。

#### 開機自動檢查與自動補齊（`env.auto_install`）

開機時一定做一次檢查；**會不會自動裝**由 `settings.json` 決定：

| 模式 | 行為 |
|---|---|
| `off` | 只檢查、只報告 |
| `safe` | 自動補 **deps／esptool**（便宜、必需）；**不會**自動裝 1–2 GB 的 IDF |
| `all` | 連 **ESP-IDF** 都自動裝（背景執行，不會阻住開機；用 `env.idf_version`／`idf_targets`／`idf_dir`） |

- 自動安裝**在背景 thread 跑**，server 照樣起；進度喺 UI「環境」頁即時睇。
- **失敗冷卻 6 小時**（`.env-auto.json`，gitignore）：避免每次開機重試拖慢啟動。
- 只缺 IDF 時會明確講「唔影響燒錄」，唔會嚇你。
- 預設就是 `all`（開機就檢查＋裝）；想只檢查 → `"env": { "auto_install": "off" }`。

#### 目標晶片：S／C／P 系列（清單由 ESP-IDF 本身提供）

> **重要（實際踩過的錯）**：晶片清單**不可以寫死**。使用者需要 `esp32s31`，
> 但寫死的清單沒有 → 程式報「唔認識嘅晶片」而令自動安裝失敗。
> 事實：IDF v6.1 的 `components/soc/` 就有 `esp32s31`（連 `c5`／`c61`／`h4`／`h21`）。

現在的做法（三層，**以 IDF 為準**）：

1. 已安裝 IDF → 讀 `components/soc/esp32*` 目錄（最快最準，不用 subprocess）；
2. 未安裝 → 讀 GitHub API 的 `components/soc`（快取 1 小時）；
3. 都失敗（離線）→ 用內建清單（含 s31／c5／c61／h4／h21，可能仍會過時）。

UI 的 chip 由 `GET /api/env` 的 `targets.live` **動態產生**（實測 13 款），
所以之後出新晶片會自動出現，不用改前端。預設勾選順序：
**UI 上次選擇 → 設定檔 `env.idf_targets` → 內建預設（S3／C3／C6／P4）**。

**未知名字唔會直接丟**：寬鬆模式（開機自動安裝／設定檔）會**保留並警告**，
交 IDF 自己判斷；只有 API 的嚴格模式才會拒絕明顯打錯的名字（並列出可用值）。

#### IDF 裝喺邊：會自動揀可寫位置

有些環境唔准寫家目錄（公司機／受控資料夾；本專案沙箱實測
`[WinError 5] 存取被拒: C:\Users\<user>\esp`）。所以：

- 候選順序：`ESPWM_IDF_DIR`／`IDF_INSTALL_DIR` → `~/esp/esp-idf` →
  `%LOCALAPPDATA%\esp\esp-idf`（Windows）／`~/.local/share/esp/esp-idf` →
  `~/.espressif/esp-idf` → **`<repo>/.idf/esp-idf`**（workspace 內，一定寫得入）→
  `%TEMP%/esp/esp-idf`；
- 會**實際試寫**一個檔案才當可用；
- **一個都唔可寫**時，開機不會白白跑 1–2 GB，而是直接講：
  `所有預設位置都唔可寫…請喺 settings.json 設 "env": {"idf_dir": "D:/esp/esp-idf"}`；
- 環境類失敗（權限／缺 git）**不記錄 6 小時冷卻**——修好設定重開就即刻再試。
- **`IDF_TOOLS_PATH` 會指去 `<idf_dir>/../tools`**：IDF 的工具鏈默認裝在
  `~/.espressif`，同樣會被受限環境擋住。指到同一個可寫位置，日後清理就係刪一個資料夾。
- 用 repo 內兜底時，1–2 GB 會落在 `<repo>/.idf/`（已 `.gitignore`）；
  想搬走就刪掉，或用「用可寫位置」揀返 `~/esp`。

```bash
# 多晶片（逗號）
curl -s -X POST http://127.0.0.1:8011/api/env/install -H 'Content-Type: application/json' \
  -d '{"what":"idf","version":"v6.1.1","target":"esp32s3,esp32c3,esp32c6,esp32p4","confirm":true}'

# 或用 preset
-d '{"what":"idf","target":"scp_mainstream","confirm":true}'
# 或用陣列
-d '{"what":"idf","target":["esp32s3","esp32p4"],"confirm":true}'
```

| preset | 內容 |
|---|---|
| `scp_mainstream` | s3／c3／c6／p4 |
| `s_only` | s2／s3 |
| `c_only` | c2／c3／c6 |
| `p_only` | p4 |
| `all` | 全部（很大） |

晶片名會驗證（不認識會列出可用值）；`all` 不可與其他混用；重複會去重。

#### ESP-IDF 的版本與「虛擬環境」實情

| 問題 | 答案 |
|---|---|
| IDF 有 Windows 版嗎？ | **有，官方支援**：`install.ps1`（本次安裝用）＋ `export.ps1`。macOS／Linux 用 `install.sh`／`export.sh`。 |
| 可以裝進 WebMaster 的 `.venv` 嗎？ | **不可以，也不需要**。IDF 會在 `%USERPROFILE%\.espressif\python_env\idfX.Y_py3.NN_env` **自建**一個 Python 環境，工具鏈在 `.espressif\tools\`。我們的 `.venv` 只服務 web server。 |
| Python 版本要求 | **3.10–3.14**（[官方 EIM 先決條件](https://docs.espressif.com/projects/idf-im-ui/en/latest/prerequisites.html)：3.14 已在 Windows／macOS／Linux 完整支援）。本機 3.14.6 ✓ |
| 有 `v6.1.0` 嗎？ | **沒有**。ESP-IDF 的 tag 慣例是 `v6.1`（發佈）＋ `v6.1.1`（修補），**沒有 `.0`**。要 6.1 系列請揀 `v6.1` 或 `v6.1.1`。 |
| 怎樣指定版本 | UI「環境」頁有**版本下拉**（由 GitHub 實時拉 tag，快取 1 小時）；API 用 `version` 參數。打 `6.1.0` 會自動翻譯成 `v6.1` 並在日誌說明。 |

```bash
# 版本清單（真實 tag）
curl -s 'http://127.0.0.1:8011/api/env/idf/versions' | python -m json.tool | head -20

# 指定版本安裝（用 v6.1.1；寫 6.1.0 會自動變 v6.1）
curl -s -X POST http://127.0.0.1:8011/api/env/install \
  -H 'Content-Type: application/json' \
  -d '{"what":"idf","target":"esp32s3","version":"v6.1.1","confirm":true}'
```

裝完之後 `make.py` 要知 IDF 在哪：

```powershell
# Windows（每次開新終端都要 export；或寫入 make_config.json 的 idf.path）
C:\Users\<你>\esp\esp-idf\export.ps1
```

```json
// make_config.json
{ "idf": { "path": "C:/Users/<你>/esp/esp-idf" } }
```

#### 實際安裝結果（本機，2026-11-18）

| 項目 | 結果 |
|---|---|
| ESP-IDF | **v6.1.1** @ `<repo>/.idf/esp-idf`（家目錄唔可寫 → 自動落到 repo 內，已 gitignore） |
| 工具鏈 | 5 個 target（s3／s31／c3／c6／p4）＋ cmake／ninja／openocd，**約 6.1 GB** @ `<repo>/.idf/tools` |
| IDF Python 環境 | `idf6.1_py3.14_env`（Python 3.14.6） |
| 結果 | 五種能力全部可用（含 `build_firmware`） |

> 體積比預期大：多 target 的工具鏈（xtensa／riscv gcc、clangd、openocd、cmake、ninja）
> 合共 **約 6 GB**。想搬去其他碟：刪掉 `<repo>/.idf`，用「用可寫位置」揀
> `D:\esp\esp-idf` 再重裝。

安裝時會自動處理兩個坑（都會被受限環境擋）：

1. `IDF_TOOLS_PATH` → 指去 `<idf_dir>/../tools`（預設喺 `~/.espressif`）。
2. `PIP_CACHE_DIR`／`TMP`／`TEMP` → 指去 `<tools>/pip-cache`、`<tools>/tmp`
   （否則 pip 建 wheel 時會 `[WinError 5]` 而整個安裝失敗，實測見過）。

#### Windows 原生 vs WSL（本機實測）

| 項目 | Windows 原生 | WSL2（ext4） | WSL2（/mnt/c） |
|---|---|---|---|
| 進程建立（300 次 `git`） | **14,980 ms** | **254 ms** | — |
| 小檔 I/O（300 個） | **180 ms** | **196 ms** | **1,583 ms** |
| hello_world 清建置 | **27.7 s** | 未裝 | — |

- 差異主要來自**進程建立**（Windows 慢約 59×），所以專案越大差距越明顯；
  小專案（hello_world）只係 27.7 s，唔算「慢好多」。
- **最差組合**：原始碼放 `C:` 但用 WSL 建置（`/mnt/c` I/O 慢 8 倍）。
- WSL 燒錄要 `usbipd-win` 轉發埠，多一層脆弱點；本工具本質係 Windows（COM＋UI）。
- 想加速（Windows 上）：開 `ccache`（IDF 支援 `CCACHE_ENABLE`）、
  把 `.idf` 同 `build` 目錄加入防毒排除。

#### `export.ps1` 的限制與替代

`export.ps1` 會用 `psutil` 偵測父 shell；在非互動／短命進程（由 server 起的子進程）
會 `psutil.NoSuchProcess` 而失敗。所以：

```powershell
# 互動終端（正常用法；要 -ExecutionPolicy Bypass）
$env:IDF_TOOLS_PATH='<repo>\.idf	ools'
powershell -ExecutionPolicy Bypass -File '<repo>\.idf\esp-idf\export.ps1'
```

工具內部唔靠 export：`env.idf_env()` 自己砌 `IDF_PATH`／`IDF_TOOLS_PATH`／
`IDF_PYTHON_ENV_PATH` 同 `PATH`，所以「檢查」同「建置」都唔需要先 export。
`make.py` 亦已識搵 `<repo>/.idf/esp-idf`（`tools/ncbuild/env.py::idf_path_for`）。

安全設計：

- **不覆蓋非空目錄**：`idf` 安裝若目標資料夾存在又非空、又不是 IDF repo → 停手並報錯
  （`rc=-2`），不會刪你的檔案。已存在的 repo **不會**自動切版本（會提示你自己換）。
- 目標晶片名會驗證（只准 `[a-z0-9,_-]`），且命令以 list 形式傳給 `Popen`，不經 shell。
- 安裝命令全部可在 `env.install_command()` 檢視（不執行）。

## 測試

```bash
cd webmaster
python -m pytest -q
```

**238 個測試**（237 passed ＋ 1 skipped；Windows 無管理員權限時 symlink 測試會 skip）：
NC4 codec／分幀、模擬裝置檔案協議、OTA、REST/WS、drive API、
共享 log／activity／AI actor、esptool 命令組裝、firmware ↔ UI 指令碼契約、C 語法 parse、
**容器解析**（`.nct` header／manifest／CRC）、**啟動腳本**（分類／平台／防穿越）、
**JSON 格式化**（正確輸出／壞 JSON 帶行欄／非 JSON 拒絕）、**設定檔**（優先次序／預設池／壞值帶 key 名／auto_port／位置在啟動器同層）、
**真板偵測**（VID 白名單／排除 AMT 藍牙／prefer_port）、
**前端模組**（載入次序／use strict／語法／$("#id") 都存在）、
**序列埠交接**（release／reclaim／409 文案／交還後重連）、
**環境自檢與安裝**（報告形狀／唔會偷裝／confirm 必填／唔覆蓋非空目錄／命令組裝）、
**AI 燒錄請求**（佇列／映像回讀／claim→result／取消／上限／同時只准一個）、
**IDF 版本**（`v6.1.0`→`v6.1` 翻譯／系列取最新修補／未知版本警告／clone 帶 `--branch`）、
**多晶片與自動補齊**（S／C／P 預設／preset／去重／未知晶片／off-safe-all 決策／6 小時冷卻）、
**新晶片容錯**（S31 接受／清單以 IDF 為準／未知名字保留而唔丟／內建清單含 c5·c61·h4·h21）、
**版本新鮮度**（PyPI 比較／離線不報錯）、**env 設定 API**（寫 local／唔改團隊檔／schema 驗證）。

> Windows 上用 `python -m pytest -q` 即可；測試**唔用** pytest 的 `tmp_path`
> （sandbox 會為它加只俾建立者的 ACL，之後 `scandir` 會 WinError 5），
> 一律用 `tests/conftest.py` 的 `workdir` fixture。

## 限制

- 一個 server 可以連多台（每台一條序列埠）；UI 用 header 的 device selector 切換，
  AI 用 `device=`／`/api/select`。唔使再為第二塊板開第二個 server。
- server 獨佔 port 時，**唔好**再開 `nc4.py`、`esptool` 或第二個終端直接連；AI 用 REST。
- 上傳係 4 KB chunk + 逐包 ACK（stop-and-wait）。
- 每次改檔後 UI 自動 `FILE_SCAN`；FAT 很大時會慢少少。
- 未做斷點續傳、`FILE_MOVE`、SD 寫入；拖完檔仍然要 `REBOOT` 才生效。
- `--sim` 用 `.simfs/` 做虛擬碟；ESPTOOL 在 sim 模式會被拒絕。
- P4 的 NC4 OTA（`0x2201` BEGIN）目前回 `err_begin_partition_conflict`：板上運行的 partition table
  不是這份 16 MB（ota_0/ota_1）表。要 P4 用 NC4 OTA，先用 ESPTOOL 燒 bootloader＋partition table；
  否則 P4 請用 ESPTOOL，S3 用 NC4 OTA。
