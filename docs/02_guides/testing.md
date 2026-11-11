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
0x1811 BENCH_READY → 清計數，回 0x1814 {ok:0}
0x1812 BENCH_DATA  → 每包合法資料 ok+1（不回覆）
0x1813 BENCH_RESULT→ 回 0x1814 {ok:N} 並清計數
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

> ⚠️ **量測入口已移除**（2026-11-04）。`DEV_BENCH` 的 erase／write 模式（最後是 `0xF001` mode 2/3/4）
> 等於讓任何通道傳一幀就能擦寫 OTA 分區，已整段刪掉；現在送這些 mode 會回 `{"err":"mode"}`。
> 下表是當時量到的數字，只作歷史紀錄；要重量請用一次性的測試 partition 或 host 端 `esptool`。

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

## 2.8 雙核 hub 交換（0xF003）

### 方法

`0xF003 HUB_BENCH`（`iters:u32 [mode:u8]`）：handler 建一個 hub（16 槽 × 64 B，SRAM），
開兩個 task —— **writer 固定 core 1、reader 固定 core 0** —— 之後：

```
writer: write_begin → 寫 counter + 60 B canary → commit
reader: read_begin  → 驗 counter 與 canary 一致 → read_end
```

兩邊都用「先自旋 400 次、太久才 `vTaskDelay(1)`」的退避策略，並帶 3 秒硬 deadline。
回覆（`0xF004`）除了 `iters/read/us/per_sec/torn`，另外帶 `w`（commit 次數）、`w_full`、
`r_null`、`exit`（1=完成 2=逾時 3=等不到 start）—— `read=0` 時靠這幾個欄位分辨是哪一邊沒動。

工具：`tools/bench/hub_bench.py --iters N [--repeat N] [--late]`。

### 結果：S3（2026-11-01，HZ=100）

| iters | 耗時 | 交換率 | 每次 | torn |
|---|---|---|---|---|
| 5,000 | 36.1 ms | 138,404 /s | 7.2 µs | 0 |
| 20,000 | 145.2 ms | 137,774 /s | 7.3 µs | 0 |
| 100,000 | 729 ms | 137,165 /s | 7.3 µs | 0 |
| 200,000 | 1457 ms | 137,259 /s | 7.3 µs | 0 |

**線性、可重現、零撕裂。**

### 結果：P4（2026-11-02，360 MHz，HZ=1000，PULL/idle 系統）

| iters | 耗時 | 交換率 | 每次 | torn | exit |
|---|---|---|---|---|---|
| 5,000 | 10.9 ms | 459,517 /s | 2.18 µs | 0 | [1,1] |
| 20,000 | 39.9 ms | 501,505 /s | 1.99 µs | 0 | [1,1] |
| 100,000 | 197.9 ms | 505,357 /s | 1.98 µs | 0 | [1,1] |
| 200,000 | 388.9 ms | 514,296 /s | 1.94 µs | 0 | [1,1] |

**P4 比 S3 快 3.6 倍**（1.9–2.2 µs vs 7.2 µs）。同一份程式、同一種量測。

⚠️ 這個數字是「系統閒著」的成本。把 NC4/TCP 切到 PUSH 收斂模式（RX task 在 core1、
prio 10，每 1 ms 醒一次）之後，同樣 20,000 次掉到 **60k–210k /s（6–16 µs）而且很不穩**：
量測 task 的 priority 是 1，會被 RX task 一直插隊，於是 `w_full` / `r_null`
從 ~7k 暴增到 40k–128k。**報這個數字時一定要註明系統負載。**

### 踩到的坑 #1（S3 時期，已修）

第一版用 `taskYIELD()` 等待空槽 —— 同優先序沒有其他 task 時它就是**空轉**，
兩個量測 task 把兩個核心佔死、IDLE 跑不到、解碼 task 排不上，**整台機器卡死**（連心跳都沒有）。

修法三件事：`vTaskDelay(1)` 取代 `taskYIELD()`、量測 task 自帶 deadline、優先序降到 1。

> 教訓：**任何等待都必須有上限**。這條同時適用於管理器本身（API 一律立即返回）。

### 踩到的坑 #2（P4：read=0，已修）

P4 上第一次跑就是 `{"read":0,"us":3000798,"exit":[3,1]}` —— 3 秒逾時、一格都沒交換。
根因**不是**「decode 佔住核心」，而是 handler 把 `deadline_us` 寫在**開 task 之後**：

```c
xTaskCreatePinnedToCore(writer, "hub_w", …);   // P4 上這條 task 立刻在 core1 跑起來
xTaskCreatePinnedToCore(reader, "hub_r", …);
b.deadline_us = now + 3s;                       // ← 太晚：writer 已經看過 deadline=0
```

writer 一啟動就檢查 `!start && expired()`，而 `deadline_us` 還是 0 → `now > 0` 成立 →
它判定「已逾時」直接結束，連 `start` 都沒等到（`exit=3` = `HUB_EXIT_NO_START`）。
reader 等不到資料，3 秒後收手 → `read=0`。S3 只是僥倖沒撞上（core1 當時有其他 task 擋著）。

修法：deadline 在開 task **之前**就寫好；task 結束原因寫進 `exit`；
`0xF003` 多一個 `mode=1`（`--late`）故意用舊順序。**它重現的是 race，不是必現**：
PULL/idle 組態下 4/4 次都重現 `read=0 / exit=[3,1]`；PUSH 組態下 core1 有 RX task 插隊，
writer 反而贏了競爭 → `read=20000`。所以這個模式是「舉證工具」，不是斷言。

> 教訓：跨核 handoff 的「初始化順序」不能靠同核的直覺。新 task 可能在 `xTaskCreate` 返回前就跑了。

---

## 2.9 每條線的緩衝 vs 慢 handler（誰才是瓶頸）

### 實驗 A：純 RX 容量（不回覆，排除 TX 干擾）

用 `0x1811/0x1812/0x1813`（BENCH 域）：先 READY，一次寫入 N 個 `BENCH_DATA`（每幀 77 B），
再用 RESULT 讀計數。

| 一次灌 | 計數 | 結果 |
|---|---|---|
| 100 | 100 | 全收 |
| 500 | 500 | 全收 |
| 1000 | 1000 | 全收 |
| **2000（154 KB）** | **2000** | **全收、零丟失** |

**結論：RX 路徑不是瓶頸。** 8 KB 的 CDC ring + decode task 的消耗速度足以吃掉 2000 幀的突發
（host 寫入會被 USB backpressure 自然節流）。之前看到的掉幀是 **TX 側**（回覆塞不進 TX ring）。

### 實驗 B：慢 handler 會不會卡住其他指令

`0xF005 SLOW(ms)`：在 handler 裡 `vTaskDelay(ms)`，緊接著送 `STATUS_GET`，量它的回覆延遲。

| 慢 handler | STATUS 延遲（改前） | STATUS 延遲（改後） |
|---|---|---|
| 閒置 | 10.8 ms | 10.3 ms |
| 50 ms | 48.1 ms | 10.8 ms |
| 200 ms | 196.6 ms | 24.5 ms |
| **500 ms** | **503.7 ms** | **10.3 ms** |

**結論：序列化點是 handler，不是緩衝。**

改法：慢工作（`0xF005`，未來還有 OTA 寫 flash、檔案傳輸）不再在 decode task 裡做 ——
改成寫進 `jobs` hub，由 `slow_worker`（core 0、prio 2）執行，完成後才回覆。

```
decode task:  解析 → 寫一個 job → 立刻返回（10 ms 內）
slow_worker:  取 job → 做慢事（可達數百 ms）→ 回覆
```

這也順帶回答了「每條線要不要各自申請暫存」：**要，但那不是為了解決慢 handler**，
而是為了吸收 P4/WS 那種高速鏈路的突發（見 §2.8 的 sizing 公式）。

---

## 2.10 UART 迴路速度（IO7/8 短接）與一核一解碼器

### 方法

IO7(TX) / IO8(RX) 外部短接。`0xF006 UART_BENCH {baud:u32, frames:u16, size:u16}`：
worker 設定 baud → 灌 N 個 `BENCH_DATA` 幀 → 量回收到幾個（CRC 已由 parser 驗過）。

UART driver 是 **lazy 初始化**（第一次測試才安裝），開機不碰 IO7/8。

### 速度（500 幀 × 77 B）

| baud | 理論 KB/s | 實測 KB/s | 送/收 |
|---|---|---|---|
| 115,200 | 11.2 | **11.2** | 500/500 |
| 460,800 | 45.0 | **44.8** | 500/500 |
| 921,600 | 90.0 | **88.9** | 500/500 |
| 1,500,000 | 146.5 | **146.9** | 500/500 |
| 2,000,000 | 195.3 | **195.8** | 500/500 |
| 3,000,000 | 293.0 | **293.7** | 500/500 |

**完全貼著 baud/10，零丟失。** 3 Mbaud = 300 KB/s，只用到解碼能力的 3%。

### 一核一解碼器（已實作）

```
core 0: decode0（CDC）+ slow_worker
core 1: decode1（UART）
共用：dispatch 表、handler（一把 mutex 保護）
```

### 並行測試：UART 滿載時 CDC 的延遲

UART 灌 3000 幀（231 KB @ 3 Mbaud，線路時間約 0.77 s），同時每 ~10 ms 送一次 STATUS：

| 指標 | 值 |
|---|---|
| CDC STATUS | 79 次，中位 **10.5 ms**，最大 45 ms |
| 閒置基準 | 10.3 ms |
| UART 收 | 2995/3000（掉 5，**0.17%**） |

**核心隔離有效**：另一核滿載時，CDC 的延遲幾乎不變。

0.17% 的丟失發生在 3 Mbaud + 並行負載下 —— RX ring 目前 4 KB。**baud ≥ 1.5 M 時建議 8 KB**。

---

## 2.11 ESP32-P4 乙太網吞吐（純 TCP，不含 NC4）

詳見 [p4-bringup.md](../03_notes/p4-bringup.md)。工具：`tools/bench/net_bench.py`。

| 項目 | 數值 |
|---|---|
| 板子 / IDF | ESP32-P4 rev 1.3（eco2）/ **IDF 6.0.3** |
| link | 100 Mbps full duplex（RMII + IP101 generic PHY） |
| 下載 PC→ESP | **7.32 – 8.08 MB/s** |
| 上傳 ESP→PC（單通道） | **6.88 – 7.24 MB/s** |
| 雙通道（core0 + core1） | 3.88 + 4.18 = **7.75 MB/s** |
| lwIP 預設視窗 | 1.10 MB/s |
| 視窗 128 KB / 98 KB | ❌ SYN 無回應 / 掉到 0.84 MB/s |

**雙通道合計 = 單通道** → 瓶頸在 EMAC，不在核心。

---

## 2.12 NC4 over 乙太網（ESP32-P4）

工具：`tools/bench/nc4_tcp.py --host <ip>`（port 6000）。兩種收斂模式（NVS `net/rx_split`）：

| 模式 | 誰讀 socket | 解析 |
|---|---|---|
| **PULL**（0） | decode task（core0，prio 4）自己 `recv` | 同一 task |
| **PUSH**（1） | 專用 RX task（core1，prio 10）→ 8 槽 × 8205 B | decode task（core0） |

### 吞吐（2026-11-02，同樣 16 MB 總量）

| 幀 | 幀數 | PULL | PUSH |
|---|---|---|---|
| 1 KB | 16,000 | 4.64 MB/s / 4,687 幀/s | **6.32 MB/s / 6,392 幀/s** |
| 4 KB | 4,000 | 4.77 MB/s / 1,218 幀/s | **6.39 MB/s / 1,630 幀/s** |
| 8 KB | 2,000 | 4.79–4.90 MB/s / 613–626 幀/s | **6.04–6.22 MB/s / 793 幀/s** |

- 三種大小差距 < 3%（同樣總量比）→ **幀大小不是瓶頸**。
- PUSH 比 PULL 快約 **29%**：recv 與解析真的分到兩個核心。
- 同時段純 TCP（port 5000）：**7.46 MB/s 下載 / 7.11 MB/s 上傳**。
  NC4/PUSH 已經貼到純 TCP 的 84%。RTT 10 ms（`STATUS` 中位），
  視窗 65 KB 的理論上限 ≈ 6.5 MB/s —— **PUSH 已經在上限上**。
- 同一條連線連續 3 輪 16 MB：4.87 / 4.54 / 4.81 MB/s（PULL），6.16 / 6.22 / 6.04（PUSH），
  2000/2000 幀、不掉速、不掉幀。
- PUSH soak：同一條連線 **10 輪 × 16.4 MB（164 MB）** → `20000/20000` 幀、
  6.05–6.52 MB/s、25.0 s、無掉速。

### 斷線恢復（回歸測試，必跑）

```
連上 → 灌 ~2 MB 的 0x1812 → SO_LINGER=0（RST）→ 立刻重連 → 送 0x1101
```

修好之後：新連線 **0.01–0.02 s 內就有 STATUS 回覆**。
修好之前：連續 3 條新連線全部沒回覆（第 3 條連 accept 都排不進去）。

### 這輪修掉的三個真 bug

| 症狀 | 原因 | 修法 |
|---|---|---|
| RST 之後再也回不了話 | PULL 的 `tcp_nc4_read` 把 `ECONNRESET` 當「讀取失敗」回 -1，**沒收 socket** → `s_tcp_nc4_fd` 永遠 ≥ 0 → accept task 卡在等它 | 錯誤路徑一律 `tcp_nc4_close()`（與 PUSH 版一致） |
| 同上，而且新連線的小指令全被吞掉 | parser 留著斷線當下的**半個 frame**；新連線的 bytes 被接在它後面，要到湊滿 plen 才會被當成壞 frame 丟掉 | `nc4_parser_reset()` + 新連線時 `nc4_bus_reset()`（順便丟掉 PUSH 的 READY 殘格） |
| `0xF00A` 說「下次開機生效」，其實永遠沒生效 | `load_net_tuning()` 跑在 `nvs_flash_init()` **之前** → `nvs_open` 必定失敗 → 一律用預設 PULL | `nvs_boot_init()` 提前；`0xF00A` 檢查 `nvs_set/commit` 的回傳 |

> 教訓：`send()` / `recv()` 的錯誤路徑要**收線**，不是只回錯誤碼。
> 另外「設定寫進 NVS 了」跟「開機讀得到」是兩件事，要分別驗。

另外兩項順手改的：decode task stack 4096 → 6144（`reply_json` 的 frame 就吃 2.1 KB）、
accept 後設 `SO_SNDTIMEO=1s`（對端不讀時 decode task 不會卡死在 `send` 裡，還握著 dispatch mutex）。

---

## 2.13 上傳 / 下載 總表（三種模式 × 有無打包解析）

### 先定義名詞

| 詞 | 意思 |
|---|---|
| **下載** | PC → 裝置（PC 發起，裝置收） |
| **上傳** | 裝置 → PC（PC 只收） |
| **打包（pack）** | `nc4_pack_into()`：9 B 表頭 + payload + **CRC32** |
| **解析（parse）** | 找 SOF、驗 CRC32、dispatch |
| **加密** | ⚠️ **本專案沒有加密**，只有框架 + CRC32（完整性）。舊文件寫「加密」= 打包 |
| **同核** | 收/解（或打/送）在同一個 task、同一個 core |
| **分核** | 兩個 task 在兩個 core，中間隔 hub |

### 三種收斂模式（NVS `net/rx_split` / `net/frame_align`）

| 模式 | 誰讀 socket | 解析 | 複製次數 | 開啟方式 |
|---|---|---|---|---|
| **PULL** | decode task（core0）自己 recv | 同一個 task | 2 | `0xF00A 0 0 *` |
| **PUSH-slot** | RX task（core1）→ 8 槽 | decode（core0）`slot_pop` + ring | 2 | `0xF00A 1 1 0` |
| **PUSH-frame（零複製）** | RX task（core1），一格一整幀 | decode 直接在插槽上解析 | **0** | `0xF00A 1 1 1` |

### 條件

ESP32-P4 rev1.3、CPU 360 MHz、FreeRTOS HZ=1000、100 Mbps full duplex（RMII+IP101）、
lwIP `TCP_WND=65535` / `MSS=1440`、**一次一條 TCP 連線**（A2/A4/A5 除外）、
TCP RTT ≈ 8–10 ms、每筆 ≈ 16 MB。2026-11-02 深夜量測，數字是 3 次同類量測的範圍。

### 表 A：純 TCP（沒有打包／解析，三種模式都一樣）

| # | 方向 | 形態 | 核心 | 速率 |
|---|---|---|---|---|
| A1 | 下載 | port 5000，裝置只 `recv` | 單核 core1 | **6.8–7.4 MB/s** |
| A2 | 下載 ×2 | 5000 + 5003 同時 | 雙核 | **9.4–9.9 MB/s**（5.5 + 4.1） |
| A3 | 上傳 | port 5001 | 單核 core0 | **7.1–7.3 MB/s** |
| A4 | 上傳 ×2 | 5001 + 5002 同時 | 雙核 | **8.1–8.5 MB/s** |
| A5 | 全雙工 | 5000 + 5001 同時 | 雙核 | **12.1–12.4 MB/s**（6.3 + 6.1） |

> **A5 = 100 Mbps 線速的 97–99%。** 單向 ~7.4 MB/s 不是 PHY、也不是 CPU，是
> **TCP 視窗 ÷ RTT**：65535 B ÷ 8.7 ms ≈ 7.5 MB/s。要單向吃滿，得加大視窗（見 §2.11：
> 98 KB / 131 KB 在 P4 上會崩），或開第二條 stream（A2）。

### 表 B：NC4 下載（PC→裝置，裝置解析）— 三種模式

| 模式 | 1 KB × 16000 | 4 KB × 4000 | 8 KB × 2000 |
|---|---|---|---|
| PULL（同核） | 4.95 | 4.77 | 4.73 |
| PUSH-slot（分核，2 次複製） | **7.48** | 7.45 | 7.55 |
| **PUSH-frame（分核，0 複製）** | 5.65 | **6.98** | **7.12–7.53** |

- 8 KB 時三種模式差 **4.7 → 7.5 MB/s（+59%）**，來源不是複製，是「收與解分核」+「不再 10 ms 輪詢」。
- **1 KB 時零複製反而慢 ~20%**：每幀兩次 `recv` + 四次 hub 進出 + 一次 kick，固定成本在
  7000 幀/s 時被放大。4 KB 以上就打平或更好。
- 表上都是 16000/4000/2000 幀全到，`resync=0`。

### 表 C：NC4 上傳（裝置打包＋送出）

| 模式 | 1 KB | 4 KB | 8 KB |
|---|---|---|---|
| 同核（同一 task 打包後送） | 5.9–6.1 | 7.0–7.4 | 6.9–7.2 |
| 分核（core1 打包 → core0 送） | 5.4–5.8 | 7.3–7.5 | 6.7–7.4 |

分核在上傳沒有好處（`send` 有 64 KB 緩衝，打包 29 MB/s 遠快於線路）。

### 表 D：純 CPU（0xF001，不碰網路）

| 動作 | 每幀 8 KB | 速率 |
|---|---|---|
| 解析 | 267 µs | **30.7 MB/s** |
| 打包 | 285 µs | **28.8 MB/s** |

成本幾乎全在 **CRC32**（≈12 cycles/byte）。

### 表 E：延遲（命令 → 回覆）

| | 中位 RTT |
|---|---|
| 修掉 10 ms 輪詢之前 | 10.1 ms |
| **之後（task notification）** | **3.6 ms**（最小 2.6 ms） |

---

## 2.14 零複製（frame-aligned）與「10 ms 輪詢」這個真兇

### 零複製怎麼做

舊 PUSH 路徑有兩次 memcpy（`slot→chunk`、`chunk→ring`）。零複製改成「**一格 = 一整幀**」：

```
RX task（core1）              decode task（core0）
  recv 9 B 表頭                 nc4_bus_frame_begin()   ← 直接拿插槽指標
  算出 13+plen                  nc4_parser_frame()      ← 原地驗 CRC + dispatch
  recv 其餘 bytes               nc4_bus_frame_end()
  湊滿一幀才 commit → kick
```

- 表頭不合法 → 9 B 視窗滑 1 byte 重新對齊（每次 9 B memmove）。
- 新 API：`nc4_header_total()`、`nc4_parser_frame()`、`nc4_bus_frame_begin/end()`、`nc4_bus_kick()`。
- 開關：NVS `net/frame_align`（預設 1）。
- 統計：`0xF008 BUS_STATS`（`tools/bench/nc4_stats.py`），`copied` 就是「為了交給 parser 做的 memcpy」。

### 效果（同一份韌體、16 MB、8 KB 幀）

| | `copied` | 每幀解析 | 吞吐 |
|---|---|---|---|
| PUSH-slot | 33,558,530 B（2 × 16 MB） | 495 µs | 7.18–7.87 MB/s |
| **PUSH-frame** | **0 B** | **406–418 µs（−18%）** | **7.41–7.53 MB/s** |

**零複製買到的是 CPU 與延遲，不是頻寬** —— 因為頻寬的天花板是「視窗 ÷ RTT」。
要驗證這件事，把 A5 全雙工拉出來看：12.4 MB/s，證明線路還有餘裕，只是單串流吃不到。

### 真的把吞吐拉上來的是「不要輪詢」

decode task 原本在 hub 剛好空的那一刻 `vTaskDelay(10ms)`。生產者卻在這 10 ms 內把 8 格填滿，
於是變成「每 10 ms 吃 8 幀」：

| | 1 KB 幀 | 8 KB 幀 |
|---|---|---|
| 輪詢（10 ms） | **0.78 MB/s**（785 幀/s） | 6.05 MB/s |
| `nc4_bus_kick()`（task notification） | **5.65 MB/s**（5362 幀/s） | **7.45 MB/s** |

同一個修法也把 `STATUS` RTT 從 **10.1 ms → 3.6 ms**（原本每一次命令都在等那個輪詢）。

### 邊界測試（`tools/bench/nc4_stats.py --edge`，必跑）

| # | 測什麼 | 結果 |
|---|---|---|
| 1 | 一幀拆 3 段送（間隔 20 ms） | OK |
| 2 | 37 B 垃圾之後接合法幀 | OK（`resync` +37） |
| 3 | 假表頭（SOF 對、`plen=65535`） | OK |
| 4 | 8 KB 幀之後立刻送指令 | OK |

### 踩到的坑

1. **`MSG_PEEK` 不能拿來做 framing**：一幀拆 3 段送時，第二次 peek 之後看不到後面的 bytes，
   2 秒後被判「表頭不完整」而關線。改成「先讀 9 B、不合法滑 1 byte」。
2. **`vTaskDelay(1)` 當退避在小幀上是災難**：hub 滿 / 沒資料時睡 1 tick，1 KB 幀的間隔只有
   0.17 ms，吞吐直接掉 8 倍。等資料要用 `select()`，等插槽要用 `taskYIELD()`/notification。
3. **`sizeof(指標)`**：sink 的緩衝從陣列改成 `malloc` 後，`recv(..., sizeof(buf), ...)` 變成
   一次讀 4 bytes → 下載掉到 0.12 MB/s。


---

## 2.15 NC4 over WebSocket（第一版，已上板）

### 實作形態（跟 TCP 不一樣）

WS 本身就是**訊息導向**，所以這條線不需要插槽、不需要 RX task、不需要 hub：

```
瀏覽器/PC ── WS binary message（= 一個 NC4 幀）──► esp_http_server
                                                   └─ nc4_parser_frame()  直接在收到的 buffer 上解析
                                                   ◄── WS binary message（回覆）
```

- 檔案：`firmware/components/sys/transport/nc4_ws.c`；URI `ws://<ip>:8080/nc4`。
- **零複製**：解析直接在 httpd 給的緩衝上做（跟 TCP 的 frame 模式同一條 `nc4_parser_frame()`）。
- 回覆走 `httpd_ws_send_frame_async()`，外部加 mutex（slow_worker 也可能回覆）。
- S3 沒有開 `CONFIG_HTTPD_WS_SUPPORT` → 編成 stub（回 `ESP_ERR_NOT_SUPPORTED`），不影響 S3 build。
- 狀態：`0x1101 keys=net` → `{"net":{"ws":{"port":8080,"fd":…,"msgs":…,"bytes":…,"tx":…,"bad":…}}}`。

### 限制（第一版，寫清楚）

| 限制 | 值 | 備註 |
|---|---|---|
| 同時連線 | **1 條** | 第二條在第一則資料幀被拒（與 TCP 同規則） |
| 一則訊息 | ≤ **24 KB**（可放多個 NC4 幀） | 超過 → 記錄 `bad` 並斷線 |
| subprotocol | 無 | 之後要接瀏覽器 client 再加 |
| 執行位置 | **httpd 的 task**（不是 decode engine） | 慢 handler 會佔住 httpd；dispatch mutex 仍保護共用狀態 |

### 測試（`tools/bench/nc4_ws_test.py`）

| # | 測什麼 | 結果 |
|---|---|---|
| 1 | 連線 + `0x1101 STATUS` | OK（首包 6–7 ms） |
| 2 | RTT × 20 | **中位 6.87 ms**（最小 5.97） |
| 3 | 連續 100 條指令 | 100/100，0.75 s（133 條/s） |
| 4 | 吞吐 1K / 4K / 8K（各 ~16 MB，batch 1 與多幀） | 見下表 |
| 5 | 一則訊息拆成 3 個 WS frame | OK（累積後解析） |
| 6 | 垃圾位元組（37 B） | OK（`bad+1`，連線活著） |
| 7 | 過大訊息（9000 B） | OK（拒絕並斷線） |
| 8 | 重連 | OK |
| 9 | 第二條 WS | OK（被拒） |
| 10 | WS 與 TCP 並存 | OK |

### 吞吐（WS vs TCP，同樣 16 MB）

WS 每則訊息都有一個**固定成本 ≈ 0.7 ms**（`httpd_ws_recv_frame` 對每一則要做 4–6 次小
`recv`，每次都會經過 lwIP 的 netconn，等於兩次 context switch）。所以：

- 小幀（1 KB）幾乎全部被固定成本吃掉；
- 解法是**一則 WS 訊息塞多個 NC4 幀**（`batching`），解析端用 `nc4_header_total()` 逐幀原地解析。

| 幀 | WS batch=1 | WS batch=N | TCP（零複製） |
|---|---|---|---|
| 1 KB × 16000 | 1.33 MB/s | **4.30 MB/s**（batch=8） | 5.65 MB/s |
| 4 KB × 4000 | 3.25 MB/s | **5.72 MB/s**（batch=5） | 7.13 MB/s |
| 8 KB × 2000 | 4.35 MB/s | **5.26 MB/s**（batch=2） | 7.40 MB/s |

（batch 上限由 `WS_MAX_MSG = 24 KB` 決定：8K 只能 2 幀、4K 5 幀、1K 8–23 幀。）

### 調校：httpd task 的核與優先序

`httpd` 預設 `prio=5`、不指定核心。改成 **core1 / prio=10**（跟 TCP RX task 同級）之後：

| | 8 KB batch=1 |
|---|---|
| 預設 | 3.94 MB/s |
| **core1 + prio10** | **4.35 MB/s**（+11%） |

### 還沒做

- 更大的 `WS_MAX_MSG`（64 KB 可以讓 8K 用 batch=8，理論上再往上；但那是 64 KB 靜態 RAM）
- 自己寫 WS（跳過 httpd 的 5 次小 recv）—— 預估可到 TCP 的 8–9 成，但要自己做 handshake/framing

### 還沒做

- 多條 WS 同時（現在刻意只收一條）。
- 二進位以外的型態（text JSON 控制）、subprotocol。
- 把 WS 的統計接進 `0x1101`（已接）；把 WS 接進未來的 decode engine（現在直接在 httpd task 解析）。
- 瀏覽器端 demo（`tools/` 目前只有 Python client）。

---

## 2.16 最終通道對比：TCP / UDP / WS（同一天、同一台板子）

### 條件

ESP32-P4 rev1.3、360 MHz、100 Mbps full duplex、lwIP `TCP_WND=65535` / `MSS=1440`、
FreeRTOS HZ=1000、PC = macOS、每筆 ≈ 16 MB、TCP RTT ≈ 8–10 ms。
三條通道都已經做過「能做的優化」：

| 通道 | 優化 |
|---|---|
| **TCP** | 分核（RX core1 / decode core0）＋ 零複製（一格一整幀）＋ task notification |
| **WS** | 訊息即幀、直接在緩衝上解析、**多幀 batching**、httpd 釘 core1/prio10 |
| **UDP** | datagram 即幀、原地解析、`UDP_RECVMBOX_SIZE=32`、**不開 IP 分段/重組** |

### 表 A：下載（PC→裝置，裝置解析）

| 通道 | 1 KB | 4 KB | 8 KB | 失敗/重傳 |
|---|---|---|---|---|
| **TCP** | 5.65 | 7.13 | **7.40** | 無（TCP 重傳） |
| **WS**（batch） | 4.30（b=8） | 5.72（b=5） | 5.26（b=2） | 無 |
| **UDP** | 1.93（掉 16%） | ✗ | ✗ | 掉包不補 |

### 表 B：上傳（裝置打包→PC）

| 通道 | 1 KB | 4 KB | 8 KB | 備註 |
|---|---|---|---|---|
| **TCP** | 6.05 | 7.43 | **7.42** | 穩定 |
| **WS** | 4.99 | — | 6.98 | 每幀一則訊息 |
| **UDP** | **8.51**（掉 0%） | ✗ | ✗ | 只到 MTU |

### 表 C：延遲與 CPU

| 項目 | TCP | UDP | WS |
|---|---|---|---|
| `STATUS` RTT（中位） | 3.56 ms | **3.73 ms**（最小 2.56） | 6.87 ms |
| 純解析（CPU，8 KB 幀） | — | 30.7 MB/s（267 µs） | — |
| 純打包（CPU，8 KB 幀） | — | 28.8 MB/s（285 µs） | — |

### 表 D：合計（TCP，作為對照）

| 情境 | 數值 |
|---|---|
| 下載 2 條 stream | 9.4–9.9 MB/s |
| 全雙工（上下同時） | **11.9–12.4 MB/s**（= 100 Mbps 的 97–99%） |

### 結論：NC4 的通道上限在哪

| 說法 | 數字 | 受限於 |
|---|---|---|
| **單一可靠通道（TCP 8 KB）** | **7.4 MB/s** | 視窗 65,535 B ÷ RTT ≈ 8.7 ms |
| 單向最高（UDP 上傳 1 KB） | **8.5 MB/s** | 只受 MTU（一包一幀）與 CPU |
| TCP 合計（雙向／雙流） | 11.9–12.4 MB/s | 100 Mbps PHY |
| NC4 解析本身 | 30.7 MB/s | CRC32 |

**所以「NC4 通道上限」≈ 7.5–8.5 MB/s（單向），不是 CPU、也不是 PHY —— 是 TCP 視窗 ÷ RTT。**
要再往上只有三條路：加大視窗（要同時加 lwIP pbuf，之前 98 KB 會崩）、
多開 stream（實測 9.4–9.9）、或走 UDP（8.5，但只限 ≤ MTU 的幀且不保證送達）。

### 該用哪一條

| 用途 | 建議 | 理由 |
|---|---|---|
| 檔案／資產／大量資料（可靠） | **TCP** | 7.4 MB/s、零丟失、有重傳 |
| 瀏覽器／Web 工具 | **WS** | 不用額外 client；batch 後 5.3 MB/s |
| 低延遲控制、發現（broadcast） | **UDP** | RTT 最低；但只放 ≤1.4 KB 的幀 |
| 雙向同時（控制＋串流） | **TCP 全雙工** | 12 MB/s 合計 |

### UDP 的兩個坑（實測）

1. **datagram 超過 MTU 會把整條路打爛**：8 KB 的 datagram 會切成 6 個 IP 片段；
   一來裝置的 IP 重組只有 10 個 pbuf、二來掉一個片段整包就沒了。實測 4K/8K 掉 33–91%。
   所以 UDP 請維持 **≤ 1400 B/包**（= 1 KB 幀 + 表頭）。
2. **開 `CONFIG_LWIP_IP_FRAG` / `IP_REASSEMBLY` 反而更糟**：送得出 8 KB，
   但重組會把 pbuf pool 吃光，連小包都開始掉。已還原成預設（關閉）。

### 量測工具

| 工具 | 通道 |
|---|---|
| `tools/bench/nc4_tcp.py`（下載）、`tools/bench/nc4_tx_bench.py`（上傳） | TCP |
| `tools/bench/nc4_ws_test.py`（RTT／吞吐／邊界 10 項） | WS |
| `tools/bench/nc4_udp_test.py`（RTT／吞吐／掉包） | UDP |
| `tools/bench/bench_all.py`（TCP 全表）、`tools/bench/nc4_stats.py`（零複製驗證） | 全部 |

---

## 3. OTA 端到端

### 方法

```bash
tools/ota.py --port /dev/cu.usbmodem* firmware/build/esp_net_core.bin --no-apply   # 只驗證
tools/ota.py --port /dev/cu.usbmodem* firmware/build/esp_net_core.bin               # 驗證 + 切換
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
tools/ota.py --port /dev/cu.usbmodem* firmware/build/esp_net_core.bin

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
| 2026-10-31 | 同上（加入緩衝管理器） | 21/21 | 9.53 MB/s | 8.8 µs | 153.2 KB/s | `nc_buf` slot 佇列 + `0x1805` 統計 |
| 2026-11-01 | 同上（加入 plan + 0x1105） | **22/22** | 9.53 MB/s | 8.8 µs | 152.9 KB/s | `nc_plan` 宣告/撇除/查詢；host 測試 6 場景 |
| 2026-11-01 | 同上（加入 hub） | 20/20 | 9.53 MB/s | 8.8 µs | 153.5 KB/s | 雙核交換 138k/s、7.2 µs、torn=0 |
| 2026-11-01 | 同上（管理器 v2：服務宣告） | **22/22** | 9.53 MB/s | 8.8 µs | 153.4 KB/s | 每 class 一次 carve、逐項失敗、hub 走 plan |

### P4 USB / MSC / 緩衝（2026-11-11，`ad9790b9`，USB 走 HS PHY pin 50/49）

全部經 CDC（`COM13`）。P4 的 Ethernet 仍未通，所以 `bench/` 裡 TCP/UDP/WS 那幾組跑不了。

| 項目 | 指令 | 結果 |
|---|---|---|
| 解碼器一致性 | `nc4_test.py --port COM13` | **9/9** |
| 一鍵自測 | `selftest.py --port COM13 --skip-ota` | **21/21** |
| `nc_buf` 自檢 | `buf_test.py --port COM13 --status` | **ok=1 fail=0**（align/fifo/full/reserve/s2s/s2p/p2s/p2p/handover/stream/abort 全 1） |
| 解碼 8 KB | `0xF001 8192 x 200` | **269.7 µs/幀、30.43 MB/s** |
| 解碼 1 KB | `0xF001 1024 x 500` | 37.1 µs/幀、27.94 MB/s |
| 解碼 64 B | `0xF001 64 x 2000` | 5.9 µs/幀、13.04 MB/s |
| RTT（CDC） | `buf_probe.py rtt` | p50 **1.624** / p95 1.789 / max 2.633 ms（n=100, lost=0） |
| CDC 裝置→主機 | `buf_probe.py txbench` | **4.35–4.38 MB/s**（1 MiB / 228 ms，chunk 4096，stalls 228） |
| CDC 主機→裝置 | `session --storm 200:256` | **3.73 MB/s**、12,654 幀/s、`rx=200 lost=0` |
| CDC 匯流排壓力 | `0xF008` | `peak=1/4`、`slot_full=0`、`crc_err=0`（4 槽 x 256 B 充足） |
| MSC 寫 1 MB / 5 MB | 主機拖檔（WriteThrough） | **62.2 / 62.5 KB/s**（16.5 s / 80.4 s，hash 驗證 OK） |
| MSC 讀 | 同上 | 命中 OS 快取（1.5 GB/s），未量到冷讀 |
| 連續運行 | `STATUS sys` | **2013 s（33.5 min）、rst=11（USB 重啟，非 panic）、heap 350,571 前後相同** |

判讀：

- **P4 的 CDC 是 HS**：兩個方向都比 S3 的 FS 快 3.5–7 倍。本文件裡「CDC 實務上限 1.02 MB/s」只適用 S3。
- **解碼 30.43 MB/s** ≈ `measurement-method.md` 記的 CPU 上限 30.7 MB/s → CPU 不是瓶頸。
- **MSC 寫 62 KB/s** 同 S3 的 65–76 KB/s 同級（FAT-on-WL 的 erase 限制），不是 P4 退步。
- 主機端**不讀** CDC 時，回覆型洪水會掉（200 幀只回 17 幀）：`cdc_send` 等 FIFO 100 ms 後
  主動丟棄（設計如此，無 flow control）；接收端本身冇丟（`0xF0FE` 路徑 `rx=200 lost=0`）。

待辦（本輪發現）：

1. **log 環太小**：`lvl=0` 只有 2048 B，P4 開機 burst 就 `peak=2017/2048`、`dropped=208`。
   建議 QUIET 改 4096（或開 `lvl=1` 的 8192）。
2. **CDC 轉寄 ESPNOW 一直失敗**：預設 routes 有 `CDC -> ["decode0","ESPNOW"]`，但 ESPNOW 冇通道，
   累計 1911 次 `ESP_ERR_NOT_FOUND`（每幀都試一次）。要嘛刪這條 out，要嘛開 ESPNOW 通道。
3. STATUS 全部已超過 2600 B 上限（`truncated=1`），要按 `keys=` 分批問。
#### 追加：CDC 上限是「每條 endpoint」，不是 PHY（2026-11-11）

做法：`CONFIG_TINYUSB_CDC_COUNT=2` ＋ `tinyusb_cdcacm_init(TINYUSB_CDC_ACM_1)`（main.c 用 `#if` 閘住，
預設 1 條），`0xF00D` 加 mode 3 = 每 4 KB 交替兩條介面；主機同時讀兩個埠。

| 設定 | 8 MiB TX（裝置側） | stalls | 換算 |
|---|---|---|---|
| 1 條 CDC（mode 0） | 1.821 s | 1,821 | **4.39 MiB/s** |
| 2 條 CDC 交替（mode 3） | 0.956 s | 956 | **8.37 MiB/s（1.91x）** |

判讀：

- **~4.2 MiB/s 是每條 bulk endpoint 的上限**（512 B x 8000 microframe/s），不是 PHY、也不是 FIFO 大小。
  加大 FIFO 冇用（stall 次數等於 chunk 數，代表 FIFO 永遠滿，滿是因為下游排程慢）。
- 加一條 endpoint 就線性加倍 → PHY（480 Mbps ≈ 60 MB/s）還遠未用盡。
- 推論：**MSC 磁碟的讀取同樣 ~4.2 MiB/s 上限**（它也只有一條 bulk IN）；寫入另有 flash WL 限制（62 KB/s），
  所以寫入時 USB 完全不是瓶頸。
- 量完已還原成 1 條（多一條 CDC 會多一個 COM port ＋ 約 12 KB RAM）。
#### 追加：`EP_BUFSIZE` 才是主旋鈕（2026-11-11，2.67 倍）

`CONFIG_TINYUSB_CDC_EP_BUFSIZE` 一直停在預設 **512**。Kconfig 原文：
「This low layer buffer has the most significant impact on performance. Set to 8192 for best performance.」

| EP_BUFSIZE | chunk | 8 MiB TX（裝置側） | stalls |
|---|---|---|---|
| 512（原本） | 512 / 1024 / 2048 / 4096 | 全部 **4.39 MiB/s**（chunk 完全冇影響） | ~1,820 |
| 8192 | 1024 | 8.78 | 911 |
| 8192 | 2048 | 9.77 | 819 |
| 8192 | 3072 | 8.79 | 909 |
| **8192** | **4096** | **11.72 MiB/s** | 682 |
| 8192 | 5120 | 7.78 | 1,026 |
| 8192 | 8192 | 7.74 | 1,033 |

- **2.67 倍**；三次重跑 11.71 / 11.72 / 11.73 MiB/s（±0.2%），非常穩。
- 峰值在 **chunk = 4096 = EP_BUFSIZE / 2**；再大反而跌（8 KB 只剩 7.7）。
  注意：EP=512 時 chunk 掃到 8 KB 都係 4.39 —— 所以「加大 chunk」本身冇用，
  要先放寬 EP buffer，chunk 才有意義。
- RTT 不變（1.61 ms）、RX（storm）亦冇變（~3.6 MB/s）→ 這個旋鈕主要幫 TX。
- 已寫入 `firmware/sdkconfig.defaults`（`TX_BUFSIZE` 同時 4096 → 8192），
  否則下次重建會退回 512。
- 注意 11.72 MiB/s 是「一次 4 KB 連續寫」的上限；實際流量由幀大小決定
  （log 管道批次 512 B、回覆是整幀），所以真實路徑會低於這個數。
#### 追加 2：單 endpoint 的天花板在哪（2026-11-11）

| 設定 | 8 MiB TX（chunk 4096） | 備註 |
|---|---|---|
| EP 512（原本） | 4.39 MiB/s | 預設值 |
| **EP 8192** | **11.72 MiB/s** | 甜點 |
| EP 10000（Kconfig 上限） | 11.72 MiB/s | **冇再快** |
| EP 16384 | 編不過 | `RX_BUFSIZE` range 是 64–10000，EP 不能大於 RX |
| EP 8192 ＋ **2 條 endpoint** | **23.37 MiB/s（1.99x）** | 仍然線性 |

判讀：

- 單 endpoint 上限 = **11.72 MiB/s = 12.29 MB/s = 98.3 Mbit/s = 正好 3 x 512 B 每 microframe**。
- buffer 由 8192 加到 10000（上限）**完全冇再快** → 這是 driver / endpoint 排程的上限，不是 buffer 大小。
- 2 條 endpoint 仍然 1.99x → 撞到的是「每 endpoint」，唔係全域（PHY 60 MB/s、USB 2.0 HS bulk 實務 30–40 MB/s 都仲未到）。
- 對照 S3：CDC（FS）1.02 MB/s ＝ FS 上限的 68%；P4 現在 12.29 MB/s ＝ HS 的 20.5%。
  相對效率仍然低，但已經是這套 esp_tinyusb 在單 endpoint 下的極限。
- 結論：**要更快只有加 endpoint（線性）或者改 driver**；加大封包／buffer 這條路已經走到盡頭。