# ESP WebMaster 上板驗收（CDC FAT）

> 2026-11-12。對象：有 ESP-IDF 的機器 ＋ S3 或 P4 板。
> UI：`tools/esp_webmaster/`；協議：[file-transfer.md](../03_notes/file-transfer.md)。
>
> **2026-11-14 更新**：序列埠由 WebMaster server 獨佔；AI／其他工具請用 REST API
> （`X-Actor: ai`），唔好再直接開 COM。共享 log／IDE／ESPTOOL 見
> [`tools/esp_webmaster/README.md`](../../tools/esp_webmaster/README.md)。

---

## 0. 前置

```bash
# 1) build + flash（有 IDF 的機）
python make.py build s3 --tests
python make.py flash s3
# 或 P4
python make.py build p4
python make.py flash p4
```

```bash
# 2) 起 UI（同一部機，板插 USB）
cd tools/esp_webmaster
python run.py --serial COM13
# 冇 --serial 也可以：開 UI 之後喺左邊揀 port
```

P4 注意：**CDC（NC4）同 OTG/MSC 係兩個孔**。要 NC4 就插 OTG 孔（HS PHY，
COM13）；JTAG 孔（COM21）只係 console。

---

## 1. 連線 / 狀態

| 檢查 | 預期 |
|---|---|
| `/api/info` | `connected:true`、`port` 正確 |
| 設備頁 `sys` | `chip` = esp32s3 / esp32p4、`fw` 有版本、`up_s` 遞增 |
| `storage` | `mode` = `app` 或 `usb`；`free` / `total` 有數字（bytes） |
| 終端 | 見到 `0x1102` 同 log 行（`I (…) …`） |

如果 `mode=usb`：FAT 在主機手上，寫入會 `err_busy`。按 **接管 FAT**
（`0x1103 {"System":{"file_mode":1}}`）再試。

---

## 2. 檔案（CDC FAT）

### 2.1 建立索引 / 檔案樹
- 檔案頁 → 按 **重建索引**（`0x200B FILE_SCAN`）
- 預期：之後讀 `/manifest.json`，列表出現 `/config`、`/pixel`、`README.txt`
- 點入 `/config` → `system.json` / `network.json` / `hardware.json`
- 預期終端：`FILE_SCAN` log；之後 `«frame 0x2006 …»`（manifest 下載）
- 之後每次上傳／儲存／刪除／confirm／undo，UI 會自動再送 `FILE_SCAN`；
  872 KB FAT、11 個檔實測約 0.8 s。
- pending 由 manifest 的 `p` 顯示；confirm 後再 scan，`p` 應變 0。

### 2.2 上傳（新檔）
1. 拖拉一個細 JSON 到根目錄（例如 `test.json`，內容 `{"a":1}`）
2. 預期：`BEGIN → CHUNK → END`，終端見 `0x2004` ACK、`0x2006`
3. 列表出現 `test.json`，**pending = 0**（本來冇舊檔）

### 2.3 覆蓋 / 自動 confirm
1. 再上傳同一檔（內容改成 `{"a":2}`）
2. **server 喺 END 後自動送 `0x2008`**；列表**唔應該**出現「待確認」
   - 終端順序：`0x2003` → `0x2006`（END）→ `0x2008` → `0x2006`（confirm）
   - `0x2006` 的 `pending=0`
3. 例外：如果 confirm 失敗，才會顯示黃色「待確認」；按 **確認** 重試。
4. 協議層的 `0x200A UNDO` 仍然存在，但 UI 正常流程唔會再有 pending 可 undo；
   要測 undo 請用 raw client 先製造 pending。

### 2.4 下載 / 編輯
1. 點 `test.json` → 編輯器打開
2. 改內容 → 儲存 → 重新整理 → 內容仍在
3. 下載 → 檔案大小／內容一致（SHA 由 `FILE_END` 驗過）

### 2.5 刪除
- 按 **刪除** → 列表消失；`0x2006 exists=0`

### 2.6 大檔
- 上傳 100–500 KB 的 `.bin`：每包 4 KB、逐包 ACK
- 預期速度：受 CDC ＋ FAT 寫入限制（大約 100–300 KB/s）
- 中途拔線：30 秒後 session 自動 abort；重新連線可再上傳

---

## 3. OTA

1. 固件頁拖 `artifacts/<board>.bin`
2. 核對 SHA-256
3. **UI 會先讀 `STATUS_GET keys=sys` 的 `chip`，同 image 檔名比對**：
   - `esp32p4` vs `esp32s3` 唔一致 → 直接拒絕
   - 舊 firmware 冇 `chip` 欄位 → 彈 warning，要你自行確認
4. 開始 OTA → 進度；完成後 APPLY
5. 板子重啟、CDC 短暫消失；重新連線
6. `sys.fw` 應顯示新版本

---

## 4. 失敗模式對照

| 症狀 | 檢查 |
|---|---|
| 開唔到 COM | server 已獨佔；先停舊 server／燒錄器。AI 用 REST，唔好直接開 COM |
| `err_busy` | `storage.mode=usb` → 先接管 FAT；或上一個 session 未清（等 30 秒） |
| `err_no_space` | `storage.free` 太細；先刪舊 effects |
| `err_sha_mismatch` | 傳輸掉包／CRC；重試由頭再上（v1 冇續傳） |
| 檔案頁空白／冇索引 | 按「重建索引」；確認 firmware 有 `0x200B`，且 `storage.mode=app` |
| 寫入後 reboot 消失 | 未 END 完成／未確認 `.bak`；睇 `0x2006` 的 pending |
| P4 冇反應 | 插錯孔（JTAG vs OTG） |

---

## 6. 救磚（錯刷／開機 loop／USB 唔再枚舉）

> 2026-11-12 真實案例：用手指 OTA 把 **S3-N16R8 映像**推去一塊 **ESP32-S3 4MB
> （Matrix 類）**，APPLY 後板子不再枚舉。`recover.py` 已成功偵測
> `ESP32-S3 / 4MB`、寫入 Matrix 映像並通過 hash 驗證；但板子停喺 ROM，
> 要**人手拔插 USB**（RTS reset 無效）才會跳去 app。

1. **拔 USB**，等 5 秒。
2. **按住 BOOT**，插 USB（或 tap RESET），放開 BOOT → 入 ROM download mode。
   - **P4**：ROM download 只喺 **USB-Serial-JTAG 孔**（COM21 類）或 UART0；
     OTG 孔未必有 ROM USB。兩個孔都試。
   - **S3**：同一個 USB 孔就會變 ROM（`303A:1001`）。
3. 睇新 port：
   ```bash
   python -c "from serial.tools import list_ports; print([p.device for p in list_ports.comports()])"
   ```
4. 用 `recover.py` 自動睇晶片／flash size，揀 `artifacts/recovery/` 對應映像：
   ```bash
   cd tools/esp_webmaster
   python recover.py --port COM25 --dry-run   # 先睇命令
   python recover.py --port COM25             # 真燒
   ```
   對應表：

   | 偵測 | recovery 目錄 | bootloader offset |
   |---|---|---|
   | ESP32-P4 / 16MB | `ESP32-P4-C6` | `0x2000` |
   | ESP32-P4 / 32MB | `ESP32-P4-ETH` | `0x2000` |
   | ESP32-S3 / 16MB | `ESP32-S3-N16R8` | `0x0` |
   | ESP32-S3 / 4MB | `ESP32-S3-Matrix` | `0x0` |

5. 燒完拔插 USB，重新開 UI 連 CDC。

> 如果 `esptool` 連唔到：確認 BOOT 時序、換線、換孔；P4 一定要試
> USB-Serial-JTAG 孔（板上兩個 USB-C 的分工見
> [p4-usb.md](../03_notes/p4-usb.md)）。

---

## 7. 要留低嘅證據

- 終端 `.log`（下載按鈕）
- `STATUS_GET keys=sys,storage` 的 JSON
- 上傳／下載的 sha256（UI 有顯示）
- 如果 build 失敗：`make.py build` 的完整 error

把以上貼返俾我，就可以對住真實行為修 firmware。
