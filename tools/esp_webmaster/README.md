# ESP WebMaster

獨立嘅 ESP 專用 NC4 工作臺（唔依賴 mp_Net-Core）。UI 跟 `mp_Tools` 的 workbench 版面：
左邊 activity rail、頂部 device strip、左側裝置面板、底部終端／活動、status bar。

**核心設計：一個 server 可以同時連多台裝置；每台由 server 獨佔自己嗰條序列埠，
Web UI 同 AI 都經同一個 API 讀同一份 log／activity。**
所以唔會再出現「你連咗我就連唔到」或者「兩邊各自開 COM port 搶線」；
亦唔需要為第二塊板再開一個 server。UI 用 header 的 device selector 切換；
AI 用 `POST /api/select` 切 active，讀指定裝置 log 再加 `device=`。
人用瀏覽器、AI 用 REST（帶 `X-Actor: ai`）。

## 啟動

```bash
cd tools/esp_webmaster
python -m pip install -r requirements.txt   # 首次：fastapi/uvicorn/pyserial/esptool
python run.py --sim                # 模擬裝置（唔使板）
python run.py --serial /dev/cu.usbmodemF0F5BD7554EC1   # 開機自動連 S3
python run.py --port 8011 --open   # 自訂 HTTP port + 開瀏覽器
# 連第二台：UI 按「連線／新增」再揀 port，或 POST /api/connect；唔使再開 server。
```

或者一鍵（自動建 venv + 裝 deps）：

```bash
python launch.py --sim
```

瀏覽器開 `http://127.0.0.1:8010/`。

## 頁面

| 頁 | 內容 |
|---|---|
| ⚙ 設備 | sys / storage / log；接管／交還 FAT、重建 FAT、除錯 log、重啟 |
| **⌨ IDE** | **檔案頁已併入這裡**：樹狀總管＋多 tab＋CodeMirror（JSON/C/Python/XML/Markdown/Shell）＋JSON lint／格式化／主題；開檔／新檔／拖拉上載／下載／刪除／確認／復原／FILE_SCAN |
| 💾 隨身碟 | 可選（有 USB 的板）：交還 FAT → 揀電腦上嘅碟 → 直接拖拉／編輯 → 接管 FAT |
| 🔥 固件 | NC4 OTA；**ESPTOOL（ROM／USB-Serial-JTAG）**、晶片探測、offset／baud／erase |
| ⌨ 指令 | 任意 NC4 cmd + hex payload + expect；底部 `>>>` 快速指令 |

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
http://127.0.0.1:8010/?file=/config/system.json#ide
http://127.0.0.1:8010/?panel=activity#ide
```

`#ide` 開 IDE；`file=` 自動開檔；`panel=activity` 預設顯示活動分頁。

## IDE

- 左邊樹由 `/manifest.json` 砌；`⟳` = `FILE_SCAN(0x200B)`；檔案有 **JSON／C／PY／MD／JS／BIN 顏色分類**。
- 多 tab（每 tab 有類型色點）、未儲存圓點、`Ctrl/Cmd-S` 儲存、`確認`／`復原`（.bak）、下載、**刪除**。
- **JSON lint**：即時標紅、gutter 標記、`JSON ✓ / ✗`；**格式化**（標準 2 空格）與 `Ctrl/Cmd-Shift-F`。
- **主題**：Bench／Material／Dracula／Nord／Solarized（離線 CSS；記在 localStorage）。
- **拖拉上載**到整個 IDE；上載／儲存都走 CDC FAT `BEGIN → CHUNK → END`，server END 後自動 `FILE_CONFIRM`。
- CodeMirror 5、lint addon、主題全部 vendor 在 `static/vendor/codemirror/`（離線可用，含 LICENSE）。
- 二進位檔會自動改為下載。

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

## 測試

```bash
cd tools/esp_webmaster
python -m pytest -q
```

**44 個測試**：NC4 codec／分幀、模擬裝置檔案協議、OTA、REST/WS、drive API、
共享 log／activity／AI actor、esptool 命令組裝、firmware ↔ UI 指令碼契約、C 語法 parse。

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
