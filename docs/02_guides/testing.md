# 測試方法與結果

> 目的：任何人（包括下一次的自己）都能照著重跑，並且知道上一次的數字。
> 每次跑完請在 §6 追加一列，不要覆蓋舊的。

---

## 0. 測試環境

| 項目 | 值 |
|---|---|
| 日期 | 2026-10-31 |
| 板子 | ESP32-S3 rev 2 · 16 MB flash · 8 MB Octal PSRAM（N16R8） |
| MAC | `d8:85:ac:d4:2b:4c` → `cid = 0x2B4C` |
| ESP-IDF | v6.0.3（獨立安裝，非 submodule） |
| esptool | 5.6.0 |
| firmware | `4e576f1-dirty`（`OTA_VERSION_RSP.fw_ver`）；量測時跑在 `ota_0` |
| 連線 | 單條 USB：MSC（磁碟）+ CDC（NC4 + console） |
| 量測點 | NC4 幀走 CDC；每次量測前 `reset_input_buffer()` |

---

## 1. 解碼器一致性

### 方法

```bash
tools/nc4_test.py --port /dev/cu.usbmodem*
```

工具自己組 NC4 幀（CRC32 與裝置同演算法），對裝置送各種畸形輸入，再從混合著 console log 的
CDC 輸出裡掃 SOF + 驗 CRC 把回覆撈回來。預設用 `0x1101 STATUS_GET → 0x1102 STATUS_RSP` 當探針。

### 判準

**9/9 全過**才算 framer 與 mp 版行為一致。任何一項失敗都代表重同步或邊界處理有誤。

### Case 與結果

| # | 送什麼 | 期待 | 2026-10-31 |
|---|---|---|---|
| 1 | 正常單幀 | 回 STATUS_RSP | ✅ |
| 2 | 兩幀塞進一次 write（黏包） | 兩幀都解出 | ✅ |
| 3 | 一幀分三段送，中間 80 ms（拆包） | 跨 feed 拼回一幀 | ✅ |
| 4 | 壞 CRC 幀 + 好幀 | 丟壞的、解好的 | ✅ |
| 5 | 垃圾中含假 `"NC"` + 好幀 | 從垃圾裡重同步 | ✅ |
| 6 | `addr = 0x1234`（非廣播非本機） | 不回 | ✅ |
| 7 | `VER = 3` + 好幀 | 丟錯版本，後面的仍解出 | ✅ |
| 8 | 宣告 `LEN = 9000`（>8192）+ 好幀 | 丟、不影響後續 | ✅ |
| 9 | 純文字雜訊 + 好幀 | 仍解出 | ✅ |

### 這些 case 對應的實作語義

- 重同步：`VER` 錯、CRC 錯、`LEN` 超上限 → **前移 1 byte** 重找 SOF（與 mp 版 `pop_frame` 相同）
- 找 SOF：從緩衝頭掃，找不到就把整個緩衝丟掉
- 位址：`addr == 0xFFFF || addr == cid` 才處理，其餘丟棄並計數

---

## 2. 效能量測（解碼）

### 方法

送 **bench 指令**（`0x18xx` 域，與 mp 版同名域）：

```
0x1801 BENCH_REQ   payload: size(u16 LE) iters(u16 LE) [mode(u8)]
                   mode: 0 = decode（預設）、1 = pack
0x1802 BENCH_RSP   payload: str_u16len JSON
                   {"size":8192,"iters":100,"us_per_frame":861.0,"mb_s":9.53}
```

裝置端做的事：組一個 size 大小的合法幀（cmd `0x7FFE`，**故意不註冊 handler**），
暖機一次，然後連餵 `iters` 次，量總時間。所以量的純粹是 **framing + CRC32 + 複製**，
不含 dispatch 之後的業務邏輯。

```bash
# 8 KB x 100 次
tools/nc4.py --port /dev/cu.usbmodem* raw 0x1801 00206400
```

### 實測（2026-10-31，S3 @160 MHz）

| 尺寸 | 每幀 | MB/s |
|---|---|---|
| 8192 B | 861 µs | **9.53** |
| 1024 B | 110 µs | 9.47 |
| 64 B | **8.8 µs** | 8.71 |

同一個 bench 也量 **pack（組包）**，同尺寸同 iters：

| 尺寸 | 每幀 | MB/s |
|---|---|---|
| 8192 B | 873 µs | 9.39 |
| 1024 B | 111 µs | 9.37 |
| 64 B | 8.5 µs | 9.05 |

**兩個方向速度幾乎一樣**（9.4 vs 9.5 MB/s）—— 因為兩邊都要算 CRC32。
這證明 C 版的瓶頸就是 CRC 本身，不是複製、不是 dispatch。

線性回歸：**每 byte 0.105 µs（≈17 cycles）+ 每幀固定 2.2 µs**。

### 與 mp 版對照

| | 來源 | 8 KB 解碼 |
|---|---|---|
| MicroPython / S3 | 舊專案 `08_performance_benchmark.md` 的 `bench_decode` | 2.58 MB/s（3.2 ms/幀） |
| C / S3 | 本文件實測 | **9.53 MB/s（861 µs/幀）** |

**3.7 倍。** 之前口頭說過「10 倍」，那是錯的，以此處實測為準。

### 判讀

- 時間幾乎全在 **CRC32**（`esp_rom_crc32_le` 是逐 byte 查表，非硬體加速），framing 本身很小。
- **不要用 8 KB 的數字推論控制指令的延遲** —— 控制指令看 64 B 那一行：8.8 µs。
- 50 FPS 每幀一條指令 = 50 × 8.8 µs = 0.44 ms/s ≈ **0.04% CPU**。真正吃掉 20 ms 幀預算的是
  WS2812 的 800 kHz 物理輸出，不是解碼。
- 要再快只有一條路：slicing-by-8 CRC（預期 2–3 倍）。等有大封包需求（影像串流）再做。

---

## 2.4 尺寸掃描（sweet point）

### 解碼 / 組包：每個尺寸跑 same iters

| 幀大小 | decode µs/幀 | decode MB/s | pack µs/幀 | pack MB/s |
|---|---|---|---|---|
| 16 B | 3.80 | 7.60 | 3.40 | 8.49 |
| 32 B | 5.50 | 8.18 | 5.10 | 8.82 |
| 64 B | 8.90 | 8.70 | 8.50 | 9.04 |
| 128 B | 15.60 | 9.06 | 15.30 | 9.20 |
| 256 B | 29.00 | 9.28 | 28.90 | 9.29 |
| 512 B | 55.80 | 9.41 | 56.20 | 9.34 |
| 1024 B | 109.50 | 9.47 | 110.70 | 9.37 |
| 2048 B | 216.80 | 9.50 | 219.60 | 9.38 |
| 4096 B | 431.60 | 9.52 | 437.60 | 9.39 |
| 8192 B | 861.00 | 9.53 | 873.50 | 9.39 |

**結論：sweet point = 1024–2048 B。**
1024 B 已經拿到 9.47/9.53 = 99.4% 的峰值吞吐，每幀只要 110 µs；
再往上加只多 0.6%，卻讓單幀延遲翻倍。

16 B 掉到 7.6 MB/s 是因為每幀固定成本（2.2 µs）佔比變大 —— 控制指令就是這個區間，
但 3.8 µs/幀 對 50 FPS 來說是 0.02% CPU，沒有優化必要。

---

## 2.5 突發（burst）完整性

### 方法

一次 write 灌 N 個 `0x1101 STATUS_GET` 幀，數回覆。這對應舊專案的
`test_proto_speed（pipe）256/256 無遺漏`。

### 結果（2026-10-31）

| 一次灌 | 回覆 | 判定 |
|---|---|---|
| 16 / 32 / 64 / 128 | 全數 | ✅ 無遺漏 |
| 256 | 223–242 / 256 | ⚠️ 掉 14–33 |
| 512 | 119 / 512 | ❌ 大量丟失 |

### 第一次跑是 147/256 —— 原因與修正

`CONFIG_TINYUSB_CDC_RX_BUFSIZE` 預設只有 **512 B**，而 256 幀 = 4352 B，
driver 的 RX ring 直接溢出。

改成 **RX 8192 / TX 4096** 之後，256 幀從 147 → 242；無損上限約 **128 幀（~2.2 KB）**。

⚠️ **改 `sdkconfig.defaults` 不會自動套用到已存在的 `sdkconfig`** —— 這次就是踩到這個坑
（改了 defaults、build 過、數字卻沒變）。要讓它生效必須 `rm sdkconfig` 再 build。
測試環境重建時記得先確認 `grep TINYUSB_CDC_RX_BUFSIZE sdkconfig`。

### 判讀

- **NC4 目前沒有 flow control**：超過約 2 KB 的突發會丟。request/response 型的用法
  （控制指令、OTA 的 stop-and-wait）完全不受影響。
- 要支援串流（影像、連續 chunk）就必須加視窗或 ACK 節流 —— 這是舊專案
  `u8_rx_slots` + ACK 那套在做的事。

---

## 2.6 OTA 區塊大小（chunk）

同一個 327 KB image，只改 `--chunk`（**已修正工具輪詢延遲之後**的數字）：

| chunk | 耗時 | 吞吐 |
|---|---|---|
| 512 B | 7.3 s | 43.7 KB/s |
| 1024 B | 3.8 s | 85.1 KB/s |
| 2048 B | 3.0 s | 105.8 KB/s |
| **4096 B** | **2.3 s** | **141.4 KB/s** |

**結論：OTA 的 sweet point 就是上限 4096 B（`OTA_MAX_CHUNK`）。**
2048 → 4096 還是有 1.34 倍，再大要動 `NC4_MAX_PAYLOAD`（8192）這個協議常數，收益有限；
真正的下一步是**視窗式 ACK**（送 4 塊收 1 個 ACK），把每塊的往返延遲攤掉。

### 三種 flash 操作的實測（`0x1801` mode 2/3/4）

| 操作 | 實測 | 換算 |
|---|---|---|
| 寫 4 KB（已擦好的區域） | **1.57 ms** | 2.6 MB/s |
| 擦 4 KB sector | **35.7 ms** | 115 KB/s |
| 擦 64 KB block | **262.6 ms** | 16.4 ms/4 KB（每 byte 比 sector erase 快 2.2 倍） |
| OTA 整體（4 KB chunk） | **28.5 ms/4 KB** | **140–142 KB/s** |

### 理論上限與實際位置

| 情境 | 每 4 KB 成本 | 上限 |
|---|---|---|
| 每個 sector 都要擦 | 35.7 + 1.6 = 37.3 ms | **110 KB/s** |
| 用 64 KB 大塊擦 + 只寫 | 16.4 + 1.6 = 18.0 ms | **227 KB/s** |
| 完全擦好的區域純寫 | 1.6 ms | 2.6 MB/s |

實測 140 KB/s **比「每 sector 都擦」的 110 KB/s 還高** —— 說明 `esp_ota_write` 對已經空白的
sector 會跳過擦除。所以現在的數字已經貼著物理下限，沒有多少空間。

**試過但無效**：在 `OTA_BEGIN` 先對整個 image 區段做 `esp_partition_erase_range`（大塊擦）。
實測花 **1210 ms**，但隨後的寫入階段**完全沒變快** —— 因為 `esp_ota_write` 仍然逐 sector 擦。
結論：pre-erase 是純成本，已移除。

### 決策：不自己管 flash（維持 `esp_ota_*`）

理論上「大塊擦 + raw `esp_partition_write` + `esp_image_verify`」可以到 227 KB/s
（現在 140），但**決定不做**：

| 面向 | 評估 |
|---|---|
| 收益 | 327 KB image 從 2.3 s → 1.45 s，省 **0.85 秒** |
| 風險 | 失去 `esp_ota_*` 的 image header / 帳務檢查，要自己用 `esp_image_verify` 補；**未來開 secure boot 或 flash encryption 時 raw write 會直接壞掉** |
| 維護 | 自己維護一段與 IDF 內部格式耦合的程式碼 |

安全性上其實做得到（只寫「非執行中」的分區 + 驗證通過才切換開機分區，掉電最多是白寫一次）。
但 0.85 秒換這些風險不划算。

**重新評估的條件**（任一成立再回來看）：
- image 超過 ~1 MB（省下的時間變成 3 秒以上）
- OTA 變成高頻操作（CI / 部署迴圈）
- 不再需要 secure boot / flash encryption

> 現在真正的上限是「erase 總量」，不是「怎麼寫」。要用到 2.6 MB/s 的寫速，
> 前提是整區已經擦好 —— 而擦那一次本身就要 1.2 秒，跑不掉。

### ⚠️ 第一次量到的 68.5 KB/s 是錯的 —— 錯在量測工具

第一次掃描得到 9.2 / 18.6 / 37.0 / 68.5 KB/s，我一度推論「每塊 55 ms ≈ flash erase」。
重測後發現**一半以上是 host 端 `tools/ota.py` 的序列埠 timeout（50 ms）造成的**：
把 `Serial(timeout=0.05)` 改成 0.01、`poll()` 從 0.1 改成 0.01 之後，同一塊 4096 直接從
68.5 → 141.4 KB/s。

**教訓**：量到瓶頸時，先確認「這個延遲是裝置造成的還是工具造成的」。
判準是換一支工具（`tools/selftest.py`）重量 —— 兩支工具差 2 倍，就證明問題在工具。

---

## 2.7 OTA 期間的指令延遲（緩衝區有沒有用）

### 方法

在 OTA 每 10 塊之後插一個 `STATUS_GET`，量往返時間；與閒置時比較。

### 結果

| 情境 | RTT |
|---|---|
| 閒置 | 5–12 ms |
| **OTA 進行中** | **中位 6 ms，最大 13 ms** |

**沒有變慢。** 因為現在是 stop-and-wait：host 等 ACK 的時候裝置是閒的，
擦寫的 28 ms 不會和指令重疊。

### 所以「加緩衝區會更順嗎」

| 目標 | 加 flash writer task + queue 的效果 |
|---|---|
| 吞吐 | ❌ 不變（erase 總量固定） |
| 指令延遲（stop-and-wait） | ❌ 沒差（本來就有空檔） |
| 指令延遲（改成視窗式連推） | ✅ 會差很多 —— 那時解碼會連續卡在 28 ms 的擦寫上 |

**結論：在 stop-and-wait 下不需要；等要做視窗式 ACK 時再一起做。**

---

## 3. OTA 端到端

### 方法

```bash
tools/ota.py --port /dev/cu.usbmodem* build/esp_net_core.bin --no-apply   # 只驗證
tools/ota.py --port /dev/cu.usbmodem* build/esp_net_core.bin               # 驗證 + 切換
```

流程照 `slave/schema/ota.json`：`0x2201 BEGIN` → 逐塊 `0x2202 WRITE`（等 `0x2204 ACK`）→
`0x2203 END`（sha256 比對 + `esp_ota_end`）→ `0x2220 APPLY`（切換開機分區 + 重啟）。

### 實測（327 KB image）

| 階段 | 結果 |
|---|---|
| CAPS | `max_chunk=4096`、`secure_boot=0`、`rollback=0` |
| BEGIN | ok |
| WRITE | 160 塊，**8.6 s → 37 KB/s** |
| END | ok（host 算的 sha256 與裝置一致，`esp_ota_end` 通過） |
| APPLY | 重啟後 `running_slot = ota_1`、`free_slot = ota_0` |

### 判讀

- **不需要 download mode、不需要按鍵** —— 這是取代「進 ROM 燒錄」的正解。
- 37 KB/s 偏慢：每塊 stop-and-wait 等 ACK，而且 ACK 與 console log 共用同一條 CDC。
  8.6 秒更新一次 firmware 可接受；要提速就加大 chunk 或做視窗式 ACK。
- 下一次 OTA 會自動寫另一個分區（`ota_1 ↔ ota_0` 交替）。

---

## 4. 怎麼重跑（照抄）

```bash
export IDF_PATH=~/esp/esp-idf && source $IDF_PATH/export.sh
cd <repo>

idf.py build

# 只有第一次（或 bootstrap）需要 download mode：按住 BOOT 不放約 5 秒
tools/flash.sh

# 之後都用 OTA
tools/ota.py --port /dev/cu.usbmodem* build/esp_net_core.bin

# 解碼器一致性
tools/nc4_test.py --port /dev/cu.usbmodem*

# 效能量測
tools/nc4.py --port /dev/cu.usbmodem* raw 0x1801 00206400   # 8192 x 100
tools/nc4.py --port /dev/cu.usbmodem* raw 0x1801 40006400   # 64 x 100
```

埠名每次插拔可能不同，用 `ls /dev/cu.usbmodem*` 確認。

---

## 5. 未測 / 待補

| 項目 | 狀態 |
|---|---|
| OTA rollback（`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` + `mark_app_valid`） | ❌ 未開，推壞 image 會卡住 |
| 位址單播（`0x1016 SET_MASTER` → 回覆填 master_cid） | ⚠️ 已實作，未做端到端測試 |
| 多通道（UART / I2C / SPI / CAN） | ❌ 框架有，轉接頭未寫 |
| MSC 拖大檔吞吐 | ⚠️ 實測 ~80 KB/s（5 MB / 65 s），wear levelling 限制 |
| 拖檔期間的心跳抖動 | ✅ 已測（p95 1.045 s，無停頓） |
| `0x100D/0x100E` 節點掃描 | ❌ 未做 |

---

## 6. 結果紀錄（追加，不覆蓋）

| 日期 | firmware | 解碼器 | 8 KB 解碼 | 64 B/幀 | OTA | 備註 |
|---|---|---|---|---|---|---|
| 2026-10-31 | `4e576f1-dirty` | 9/9 | 9.53 MB/s | 8.8 µs | 37 KB/s | 首次 baseline |
| 2026-10-31 | 同上（CDC ring 加大後） | 9/9 | 9.53 MB/s | 8.8 µs | 37 KB/s | burst 無損上限 ~128 幀 |
| 2026-10-31 | 同上（chunk 4096） | 9/9 | 9.53 MB/s | 8.8 µs | 68.5 KB/s | 尺寸掃描完成（此列數字含工具延遲，見 §2.6） |
| 2026-10-31 | 同上（修正工具輪詢） | 20/20 | 9.53 MB/s | 8.8 µs | **141.4 KB/s** | `tools/selftest.py` 一鍵自測上線 |
