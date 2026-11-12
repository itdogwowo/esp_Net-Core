# ESP WebMaster

獨立嘅 ESP 專用 NC4 工作臺（唔依賴 mp_Net-Core）。UI 跟 `mp_Tools` 嘅 workbench 版面：
左邊 activity rail、頂部 device strip、左側裝置面板、底部終端、status bar。

**為什麼係 CDC 優先：** 部分 MCU 冇 USB driver，唔會枚舉成隨身碟（MSC），
唯一通道就係 USB CDC。所以檔案一律可以經 **NC4 FAT 指令** 操作：

| 通道 | 用途 |
|---|---|
| **USB CDC（NC4）** | STATUS、log 終端、任意指令、OTA、**檔案（LIST/READ/BEGIN/CHUNK/END/CONFIRM/DELETE/UNDO）** |
| USB MSC（可選） | 有 USB driver 嘅板：交還 FAT → 電腦見到碟 → 拖檔。冇 USB 嘅 MCU 用唔到 |

## 啟動

```bash
cd tools/esp_webmaster
python run.py --sim                # 模擬裝置（唔使板）
python run.py --serial COM13       # 直接連 CDC（P4 OTG / S3 USB）
python run.py --port 8011 --open   # 自訂 HTTP port + 開瀏覽器
```

或者一鍵（自動建 venv + 裝 deps）：

```bash
python launch.py --sim
```

瀏覽器開 `http://127.0.0.1:8010/`。

## 五頁

| 頁 | 內容 |
|---|---|
| ⚙ 設備 | sys / storage / log；接管／交還 FAT、重建 FAT、除錯 log、重啟 |
| 📁 檔案 | **CDC FAT**：目錄樹、拖拉上傳、下載、內嵌編輯器、`確認 / 復原`（.bak）、刪除 |
| 💾 隨身碟 | 可選（有 USB 嘅板）：交還 FAT → 揀電腦上嘅碟 → 直接拖拉／編輯 → 接管 FAT |
| 🔥 固件 | 拖 `.bin`、SHA-256、OTA 進度、APPLY |
| ⌨ 指令 | 任意 NC4 cmd + hex payload + expect；底部 `>>>` 快速指令 |

## CDC 檔案流程

1. USB 插住板，UI 連 CDC（例如 COM13）。
2. 「檔案」頁 → **重建索引**（`0x200B FILE_SCAN`）→ 讀一次 `/manifest.json` 砌樹。
3. 拖拉檔案上傳：`BEGIN → CHUNK×N → END`（每包 4 KB，等 ACK）。
4. 覆蓋舊檔時 device 先留 `<path>.bak`；**server 喺 END 成功後即刻自動送
   `0x2008 FILE_CONFIRM`**（用戶唔使按），所以正常情況下唔會見到「待確認」。
   只有 confirm 失敗（FAT 錯誤）才會保留 pending，UI 可再按 **確認** 重試。
5. 下載／編輯：`FILE_QUERY` 拿 size → `FILE_READ` 逐段讀 → 文字檔可即改即存
   （儲存經同一 PUT，亦會自動 confirm）。
6. 寫入需要裝置擁有 FAT；如果 `storage.mode=usb`，先按 **接管 FAT**
   （`0x1103 {"System":{"file_mode":1}}`）。

> 協議細節（payload、.bak、pending、錯誤碼）見
> [`docs/03_notes/file-transfer.md`](../../docs/03_notes/file-transfer.md)；
> 上板驗收步驟見
> [`docs/02_guides/esp-webmaster-test.md`](../../docs/02_guides/esp-webmaster-test.md)。

## 協議

| 指令 | 用途 |
|---|---|
| `0x1101` STATUS_GET | sys / storage / log |
| `0x1103` STATUS_SET | `System.file_mode`（0=交還、1=接管）、`fs_format`、`log_debug_mode` |
| `0x100F` REBOOT | 重啟 |
| `0x2005` FILE_QUERY | 單檔 metadata（存在／size／sha／free／pending） |
| `0x2001–0x2004` | 上傳（BEGIN/CHUNK/END/ACK） |
| `0x2007` FILE_READ | 下載（回 `0x2002`） |
| `0x2008/0x2009/0x200A` | CONFIRM / DELETE / UNDO |
| `0x200B` FILE_SCAN | 重建 `/manifest.json`（UI 一次過讀清單；冇 directory listing 指令） |
| `0x22xx` OTA | 固件更新 |

所有 device 操作經一把 lock 序列化；log 行（`0x1E`）同 NC4 幀由同一個嚴格分幀器分流。

## 真機驗證（2026-11-12，ESP32-S3 4MB / COM24）

- `0x200B FILE_SCAN`：11 個檔 → manifest 1,227 B，約 **0.8 s**
- 上傳 0 / 1 / 10 B：全部 ACK、SHA 正確、下載 byte-identical
- 覆蓋 → `pending=true`；confirm → `pending=false`；undo → 回復舊內容；delete → 清 manifest
- UI（`--serial COM24`）：上傳、刪除、自動 rescan、manifest 樹全部正常
- 過程中修到兩個真機 panic（`rst=4`）：decode 4 KB 棧被 manifest cJSON/FAT 打爆；
  現在 handler 只做 FAT，manifest 只由 `FILE_SCAN` 寫

## 測試

```bash
cd tools/esp_webmaster
python -m pytest -q
```

**37 個測試**：NC4 codec／分幀、模擬裝置檔案協議、OTA、REST/WS、drive API、
firmware ↔ UI 指令碼契約（3 個）、C 語法 parse（1 個）、OTA chip guard（1 個）。

## Build 驗證（2026-11-12）

WSL ＋ ESP-IDF v6.1，實際 build 過：

| Board | 指令 | 結果 |
|---|---|---|
| ESP32-S3-N16R8 | `make.py build ESP32-S3-N16R8 --tests` | ✅ 646.5 KB，分區剩 84% |
| ESP32-S3-N16R8 | `make.py build ESP32-S3-N16R8` | ✅ 511.3 KB，分區剩 88% |
| ESP32-S3-Matrix | `make.py build ESP32-S3-Matrix` | ✅ 491.3 KB，分區剩 68%（4MB、無 PSRAM） |
| ESP32-P4-ETH | `make.py build ESP32-P4-ETH` | ✅ 1006.8 KB，分區剩 75% |
| ESP32-P4-C6 | `make.py build ESP32-P4-C6` | ✅ 973.8 KB，分區剩 76%（16MB） |

> 過程中順手修了 `nc_rgb_matrix.c` 的 `#else` 重複定義（`s_demo` 未宣告／
> 同一函式定義兩次）—— 係之前 commit 帶入、會令任何 build 失敗的 regression。

### 救磚

如果錯刷了別的板（P4/S3 都會枚舉成 `303A:4003`），按住 BOOT → tap RESET
入 ROM mode，然後：

```bash
python recover.py --port COM25 --dry-run
python recover.py --port COM25
```

`recover.py` 會用 esptool 自動偵測晶片＋flash size，揀
`artifacts/recovery/<board>/` 的 bootloader／partition／app 重刷。
詳細步驟見 [`docs/02_guides/esp-webmaster-test.md`](../../docs/02_guides/esp-webmaster-test.md) §6。

## 限制

- 一個 COM port 只可以一個程式佔用；燒錄器／其他終端開住會連唔到（UI server 同 OTA 工具要輪流用）。
- 上傳係 4 KB chunk + 逐包 ACK（stop-and-wait），速度受 CDC 同 FAT 寫入限制。
- 每次改檔後 UI 會自動 `FILE_SCAN`（0.8 s / 11 檔）；FAT 很大時會再慢少少。
- 未做斷點續傳、`FILE_MOVE`、SD 寫入；拖完檔仍然要 `REBOOT` 才生效。
- `--sim` 用 `.simfs/` 做虛擬碟。
