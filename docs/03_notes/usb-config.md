# USB 配置：能力、傳輸與 CDC 基線

> 2026-11-12。檢查 CDC FAT 更新（`2084ef0`）之後，USB 這層的現況與目標模型。
> 2026-11-13：出廠預設、啟動次序、貼身緩衝已落地 —— 先看 **§0.5**（它取代下面 P0 的止血描述）。
>
> 一句話：**板子事實（有冇 USB、用邊條 PHY、邊個介面接出來）是編譯期；
> 用唔用、點用，才是執行期 config。**
>
> 基線：**任何型號第一次 bring-up，第一步都是一條可用的 CDC（NC4）**。
> MSC（隨身碟）只是「這塊板有 OTG ＋ 有 FAT ＋ 想交碟」時的可選項。

### 0.1 術語：線（line）、driver、預設 USJ

「USB」在專案裡橫跨三層；不分清楚，`usb.rx`、`CDC`、`usb_jtag` 就會互相打架。

| 層 | 叫法 | 出現在哪 | 由誰決定 |
|---|---|---|---|
| **線**（板子上的一條 USB PHY／接口） | USB 線 `usb` | `usb.rx`、`usb_core`、`usb.msc`、`usb.cdc_log` | 板子（有幾條線）＋ config（用哪條） |
| **driver／transport** | USB-OTG（TinyUSB）或 USB-Serial-JTAG（USJ LL） | `links[].kind`；`nc_usb_mode_t` | config；**預設一律 USJ** |
| **NC4 通道** | 這條線上的一條 link（`CDC`／`usb_jtag`） | `links[]` 的 `name`／`enable` | config |

六條規則：

1. **一條線只會有一個 driver**。同一條線不會同時跑 TinyUSB 與 USJ；
   換 driver = 改 `kind` ＋ 重啟。
2. **預設一律 USJ**：凡是有 USJ 的線，出廠就是 `usb_jtag`；**只有用戶手動把
   `kind` 改成 `cdc`** 才走 OTG。S2 冇 USJ，所以只有 CDC（那不是「選 OTG」，
   是沒有選擇）。
3. **有幾條線，看板子**：
   - **S3**：OTG 與 USJ 共用同一條 FSLS PHY → **一條線**，二選一。
   - **P4**：三條獨立線（USJ FS 24/25、OTG FS 26/27、OTG HS 49/50）→ 每條可各自
     配一個 driver（例如 USJ console ＋ OTG CDC）。
   - **現況**：固件仍只揀一條 USB link（`usb_nc4_link()` 取第一條 `enable`，
     多過一條警告）；P4 的多線並存屬 Phase 2（`usb.interfaces[]`，見 §2.2–2.4）。
4. **`usb.*` 前綴指「這條線」**，不是 driver：`usb.rx`（收幀環）、`usb_core`
   （所屬核心組）、`usb.msc`／`usb.cdc_log`（兩個開關）。
5. **資源名不按 driver 分**：收幀環叫 `usb.rx`，不叫 `cdc.rx`／`usb_jtag.rx`。
   ⚠️ 今日只有一條 USB 線，所以一個 `usb.rx` 就夠；P4 多線並存時**每條線要自己
   一份**，命名到時按線分（不能共用同一個 `usb.rx`）。
6. **兩個開關只對 OTG 有意義**：MSC（交碟）與獨立 log 管道（`cdc_log`）。
   USJ 冇 MSC；log 與 NC4 共用 TX（arbiter），`cdc_log` 無效。

生命週期：`usb_core` 在 B05 由「線 → 組」表寫入；`usb.rx` 在 B07 宣告、B08 carve、
B09 租用（交給 nc4 任務）；USB 真正上線（TinyUSB 列舉／log 管道）在 B10。

> log 由 B01 就在產生（走 console）；B10 開的是**管道出口** —— 兩者的時間線與板子差異見
> [boot-log.md](boot-log.md)。

**一次開機只初始化一次**：B03 先讀 config 決定這條線用哪個 driver，B10 才 init
（OTG 裝 TinyUSB；USJ 唔使裝，本身已經係 CDC-ACM，只開 log 管道）。
之後**沒有 runtime 切換 API**；要換線就改 `links[]`（或 `usb.*`）再重啟 ——
下次開機 B03 用新 config 重算，B10 用新 driver init。init 失敗不會在同一次
開機重試（USB 這次就係冇）。

兩種 transport 的完整能力對照見 §2.0，這裡只釘死「USB 這個字指哪一層」。

---

## 0. 現況：三層，但 USB 只落在其中兩層

| 層 | 位置 | 現在決定什麼 | 問題 |
|---|---|---|---|
| **能力／編譯期** | `main/Kconfig.projbuild` 的 `NETCORE_USB_OTG` | 「呢個 target 有冇 OTG」→ 整組 TinyUSB 編唔編 | 只看 `IDF_TARGET_*`，不看板子；P4 就算 `features.usb=false` 都照 y |
| **板子／編譯期** | `ports/<board>/sdkconfig.defaults` | OTG 走 FS/HS（`NETCORE_USB_OTG_HS`） | 手寫、沒有 profile、`board.py` 不驗；`board.json` 的 `features.usb` 完全冇人讀 |
| **執行期** | `system.json` 的 `usb.{msc,cdc_log}` ＋ `nc4.links[]` | 交唔交碟、log 走唔走 CDC、CDC 通唔通 | `cdc_log` 只管 OTG；`msc` 預設 1；CDC 通道藏在 `links[].enable`，不是一個「USB 基線」概念 |

兩條 transport 在 S3／P4 都會編入（USJ 看 `SOC_USB_SERIAL_JTAG_SUPPORTED`），
但**執行期只會 init 一條**（`usb_nc4_link()` 只取第一條；同一條 PHY 的板子
也只能二選一 —— 見 §0.1）：

| transport | 晶片 | 程式位置 | 條件 |
|---|---|---|---|
| OTG CDC（TinyUSB） | S3／S2／P4 | `main.c` `usb_install()` | `NC_HAS_USB`（= `CONFIG_NETCORE_USB_OTG`） |
| USB-Serial-JTAG CDC | C3／C6／H2；S3／P4 也可選 | `main.c` 的 `usb_jtag_*` | `SOC_USB_SERIAL_JTAG_SUPPORTED`（不依賴 `NC_HAS_USB`） |

所以「大部分 ESP 都有 CDC」這件事，硬件上係真；但**config／板子／驗證**三邊都未有一致的基線。

### 0.5 2026-11-13 已落地：出廠預設由晶片能力砌

`nc_cfg_chip.c` 是新的**編譯期**能力表（數據來自 IDF v6.0.3 的 soc_caps.h）：

| target | OTG | OTG HS | USJ | 出廠 USB link | kind | 核 |
|---|---|---|---|---|---|---|
| esp32s3 | ✅ | — | ✅（共用 PHY） | `usb_jtag`（可改 `cdc`） | usb_jtag | 2 |
| esp32s2 | ✅ | — | — | `CDC`（冇 USJ 可選） | cdc | 1 |
| esp32p4 | ✅ | ✅（UTMI） | ✅（獨立） | `usb_jtag`（可改 `cdc`） | usb_jtag | 2 |
| esp32c3／c5／c6／c61／h2 | — | — | ✅ | `usb_jtag` | usb_jtag | 1 |
| esp32 | — | — | — | （冇） | — | 2 |

未知晶片（表裡未有）：`main` 用**編譯期能力**砌一個臨時表 —— 有 USJ 就用 USJ，
冇 USJ 才用 OTG，兩樣都冇就冇 USB；不再盲猜 CDC。

開機次序（`app_main`）：

```text
⓪ 晶片能力 → nc_cfg_defaults_set_chip()   ← 只決定「缺檔時用什麼」，未讀任何檔
① nvs（boot 記錄：provisioned／fmq／fw_hash）
② storage：掛 FAT →（第一次或明確重建才 seed）→ nc_cfg_boot() 讀 config
③ capability（B03，第一個決定，內部次序固定）：
     (a) 核數（nc_core_probe）
     (b) 單核 → 把 config 的兩核要求併成一核（只改 RAM）
     (c) USB 人格：CDC（OTG）定 USB-Serial-JTAG；log／msc 兩個閘
④ hwcheck → hardware → declare → commit → start → usb → channels
```

B05 的 hardware 在建立「線 → 組」表時，會把 **USB 那條線的組**寫入
`ctx.usb_core`（一次）。之後 B07（回覆幀）、B10（TinyUSB／log 管道）、
B11（bus＋producer）只讀 ctx，不再各自去問一次 router ——
人格（B03）與組（B05）是兩個問題，不是重複決定。

② 與 ③ 的分工：`B02` 只負責**讀**（JSON → struct，之後全系統只讀 struct）；
`B03` 負責**落地**「這塊板做得到什麼」——核數是硬體事實，USB 是人格。
兩者都只決定，不建立任何 driver／task。

單核的併核在 ③(b)、**早過 USB 的決定**：
`routes[].out[]` 的 `decode1` → `decode0`、`tasks[].affinity` 的 core1 → core0
（`nc_cfg_merge_cores()`，只改 RAM，`/data/config/*.json` 一個 byte 都唔郁）。
之後 router、decoder 個數、任務管理器、USB 服務組全部讀同一份已收斂的表，
唔會各自再夾一次、各自再印一句。

**之後所有宣告（緩衝、bus、USB 服務組）只認 config 的 struct，不再碰檔案。**

四條規則：

1. **config 先於任何服務**：`nc_cfg_boot()` 在 `declare`／`start`／`usb` 之前完成；
   之後全系統只讀 struct。今次只把「晶片能力」提前到階段 ⓪，讓預設本身就是對的。
2. **出廠＝USJ 先（所有人的預設）**：凡是有 USB-Serial-JTAG 的晶片（S3／P4／C3…）
   預設一律用 `usb_jtag`，不是 OTG；想用 OTG 要自己改 `links[].kind`。
   只有 OTG、冇 USJ 的 S2 才用 `CDC`。`usb.msc` 預設 **0**（開機唔交 FAT）。
   想用 OTG（CDC＋MSC）就改 `links[].kind = "cdc"`（link 名也改成 `CDC`）再重啟 ——
   這就是「換 driver」。**同一條線**上兩條 driver 不會同時 init：S3 只有一條線，
   所以整顆晶片只能二選一；P4 有多條獨立線，可以各自 init（Phase 2）。
3. **冇登記＝唔佔**：`links[]` 冇這個名（或 `enable:0`）→ 唔宣告緩衝、唔建 bus；
   CDC／MSC／log 三樣都冇人要 → **連 TinyUSB 都唔裝**（省 1,280 B task 棧＋driver heap）。
   以前是反的：把 CDC 從 `links[]` 刪掉，`link_enabled()` 仍然回 true，8 KB 環照樣佔住。
4. **貼身數字**：USB parser 緩衝 = `NC_CFG_USB_RX_REGION` = **4,128**
   ＝ 4,096（FILE_CHUNK 上限）＋ 9（NC4 header）＋ 6（chunk header）＋ 4（CRC）
   ＝ 4,115 向上 32 對齊。用戶唔應該需要填 `rx.region`；要塞更大 chunk 才改 config。

**實測（2026-11-13，兩塊真板）**

| 板 | transport | region | 4 KB 上傳 | `max_dlen` | `bad_hdr`／`crc_err` | 備註 |
|---|---|---|---|---|---|---|
| ESP32-C3-DevKitM（4 MB，全清後第一次開機） | USJ（pull） | 4,128 | 0.94 s | 4,102 | 0 / 0 | 出廠 seed 自動是 `usb_jtag`、`msc=0`；4 KB／3 KB／2 KB／再 4 KB 全部 SHA 對 |
| ESP32-S3-N16R8（`kind=cdc`，OTG） | TinyUSB CDC（push-slot） | 4,128 | 0.35 s | 4,102 | 0 / 0 | `peak` 1 格、`slot_full` 0 |
| ESP32-S3-N16R8（`kind=usb_jtag`，USJ） | USB-Serial-JTAG（pull） | 4,128 | 0.98 s | 4,102 | 0 / 0 | 與 C3 同一個 driver；`rt` 冇 TinyUSB／cdc.rx |
| ESP32-P4-C6（`usb_jtag`，USJ） | USB-Serial-JTAG（pull） | 4,128 | —（1.06 MB OTA 用 175 s，≈6 KB/s） | — | — | 2026-11-14 實測；主機 raw write 4 KB = 640 ms（64 B × 10 ms tick），與 C3／S3 的 USJ 同級；CDC 約 97 KB/s |

兩塊板都係：payload 4,102（6 + 4,096）→ 幀 4,115 → 4,128 的環 **零丟幀**。
S3 的 `/config/system.json` 是舊版（冇 `usb`、冇 `rx`），reboot 後仍然是 4,128 →
證明「缺欄位 → 用預設」真的在每次開機發生，不是 seed 一次性的假象。

**「冇登記就唔佔」實測（同一塊板，只改 config）**

| 板／config | `mem.plan` 內部 bytes | 條目 | `rt` 的服務 |
|---|---|---|---|
| S3：`usb_jtag` ＋ UART | 10,382 | reply0/1、`usb.rx` 4,128、`uart.rx` 1,024 | decode0/1、`uart.rx` |
| S3：`usb_jtag`，**刪掉 UART** | **9,358**（−1,024） | reply0/1、`usb.rx` | **冇 `uart.rx`、冇 `decode1`** |
| C3：`usb_jtag` ＋ UART | 7,767 | reply0、`usb.rx`、`uart.rx` | decode0、`uart.rx` |
| C3：`usb_jtag`，**刪掉 UART** | **6,743**（−1,024） | reply0、`usb.rx` | decode0（冇 `uart.rx`） |
| S3：`kind=cdc` | 10,382 | reply0/1、`usb.rx` 4,128、`uart.rx` | 有 `TinyUSB`、`cdc.rx`；**冇 usb_jtag bus** |

另一邊（選 `usb_jtag` 時）`rt` 完全冇 `TinyUSB`／`cdc.rx`／`msc_async_io` ——
未選的 transport 連 task 都唔會建立，唔止唔佔緩衝。

> **S3 的 PHY mux 陷阱（2026-11-13 實測）**：OTG/USJ 的 mux 在 RTC domain，
> **軟重啟（0x100F）不會重設**。上一次開機跑過 TinyUSB，改成 `usb_jtag` 再軟重啟 →
> USJ 完全不會枚舉（主機見不到裝置）。修正：每次選 USJ 都呼叫
> `usb_serial_jtag_ll_phy_enable_external(false)` 把 mux 指返 USJ（`main.c` 的 `ph_usb`）。
> 修好之後同一個切換（cdc ↔ usb_jtag）雙向都通過，軟重啟亦保持可見。

> 未測：P4（OTG FS/HS 與 USJ 三條 PHY）。P4 走的是與 S3 相同的 TinyUSB 分支，
> 但 PHY／接線不同，仍要實板驗。USJ 那條在 P4 已編入（`SOC_USB_SERIAL_JTAG_SUPPORTED`），
> 只是未上板實測。

**已知成本**：`usb.msc=0` 仍然會安裝 MSC driver（`msc_async_io` task）。
TinyUSB 的 descriptor 是編譯期決定的（`CONFIG_TINYUSB_MSC_ENABLED`），
跳過 driver 會令 MSC interface 有 descriptor 但沒有 backing —— 2026-11-13 實測
CDC 模式會 reboot loop。要真正省掉，要連 descriptor 一起關（Phase 2）。

單核板（C3／C6／C61／C5／H2／S2）：實際只建 1 個 decoder（上限仍然是 2）。
C3 沒有 PSRAM，第二份回覆幀 2,610 B 是**內部 RAM**，以前由頭到尾冇人用。
`routes[]` 寫 `decode1`、或 `tasks.json` 寫 core1：B03 capability 一次過併到 core0
（單核根本沒有 core1），只改 RAM、唔寫盤。firmware 自己宣告的 system loop
（`decode1`、fx 等）仍然是 `nc_core_clamp()` 在 B09 收斂 —— 那些是程式碼，
不是 config，租約管不到它們。

仍未做：`usb.interfaces[]` 多介面 schema（§2.2–§2.4）、`max_chunk` 協商（§2.5）、
P4 的 JTAG 第二條 link。今次只落地「出廠預設 ＋ 註冊語義 ＋ 貼身緩衝」。


---

## 1. 檢查發現（依風險）

### P0 — C3（JTAG CDC）的 4 KB 上傳會失敗：協議假設 vs 實際 region（已止血，正解見 §2.5）

- 4 KB 只是 host 的預設 chunk（`tools/esp_webmaster/files.py:118`），
  **不是協議要求**；2 KB、3 KB 一樣合法。真正的不變量是：
  `frame = 9 + (6 + chunk) + 4 = chunk + 19` 必須 ≤ rx parser region。
- `usb_jtag` 以前沒有專屬的 rx 預設，跌落 `default` 的 **4,096**；
  它最大只收到 4,077 B 的 chunk，但 host 送 4,096（幀 4,115）——
  差 19 B 放唔落，parser 在 `nc4.c:659` 當「幀過大」跳過，主機只見到 timeout。
- 問題本質不是「region 要幾大」，而是 **host 從來冇問 device 可以收幾大**。
- 這正是「冇 OTG、只有 CDC」的板子，亦是 CDC FAT 這個功能存在的理由。

止血（已做；數字已由 §0.5 取代）：

- `nc_cfg_rx_default()` 加 `NC_LINK_USBJTAG` 專屬預設，最初 4,192、現為 **4,128**；
- C3 的 `system.json` 唔再寫 `rx.region`（改用預設 4,128，證明用戶唔使填）；
- `main.c` 兩處 fallback 對齊（與實際宣告一致）。
- 已用 ESP32-C3 建置驗證（`NETCORE_USB_OTG=n`、`SOC_USB_SERIAL_JTAG=y`）。

> **+19 不是 +17**：FILE_CHUNK 的 payload header 是 `file_id(u16) + offset(u32)` = 6 B，
> 所以幀 = 9 + 6 + chunk + 4 = **chunk + 19**；OTA_WRITE 只有 `offset(u32)` = 4 B，
> 才是 chunk + 17。舊註解兩邊都寫 +17，正是 4,096 → 4,115 少 19 B 那個 bug 的來源。

> 這只是把天花板抬到 4 KB，令現時 host 的預設放得落；**唔係協議的一部分**。
> 長遠要按 §2.5 做 `max_chunk` 協商，host 的 4 KB 只是一個「preferred」值。

### P0 — P4-ETH 會把 FAT 交俾一個唔存在的 host

- `board.json` 寫 `features.usb=false`，但 P4 的 `NETCORE_USB_OTG` 按 target 預設 **y**
  （`Kconfig.projbuild:11`），`sdkconfig.defaults` 也沒有關。
- 它的 `system.json` **冇 `usb` 段** → `usb_msc` 預設 **1**（`nc_cfg_defaults.c:91`）。
- `ph_usb()` 於是會呼叫 `storage_hand_over_to_usb()`（`main.c:3898`），
  把 FAT 從 APP 交出去；但這塊板沒有接 OTG，主機永遠唔會來。
- 結果：裝置自己失去 `/data` 的寫入權（只剩唯讀視圖），檔案指令回 `err_busy`。
- 這一條是**用現時 config 推導出來的必然路徑**，未在 P4-ETH 硬件上覆核；
  但正正是「board fact 冇接到 build 與 runtime」的典型。

### P1 — P4 的三條 PHY，firmware 只認一條

- P4 的 USB-Serial-JTAG（FS_PHY1，GPIO24/25）同 OTG（HS，pin 50/49）是**獨立 PHY**，
  可以同時用；但 `usb_jtag` 那條 transport 只在 `!NC_HAS_USB` 時編譯
  （`main.c:931`、`main.c:2058`、`main.c:4072`）。
- 所以 P4 的 303A:1001 那條線永遠只做 console，不能做 NC4。
- 反而 C3 這種「只有 JTAG」的板子可以。P4 是「多個 USB 接口」最複雜的一塊，
  現在卻是能力最少的一塊。

### P1 — MSC 開機預設與「CDC 優先」相反（**2026-11-13 已改：預設 0**）

- ~~`usb_msc` 預設 1~~ → 現在預設 **0**（`nc_cfg_defaults_system()`）：開機先把 `/data` 留在 APP。
- 即使冇插線、冇 host，裝置都會失去 `/data` 寫入權；CDC 檔案工作臺第一步
  往往要「接管 FAT」。
- 如果 CDC 是基線、MSC 是可選，較合理的預設是 **`msc=0`**（裝置持碟），
  想用隨身碟時才明確交還。最起碼要由板子宣告，而不是全域預設 1。

### P1 — 板子事實自相矛盾

| 板子 | `board.json` | `sdkconfig.defaults` | 實際 |
|---|---|---|---|
| ESP32-P4-C6 | `features.usb = "otg-fs"` | `CONFIG_NETCORE_USB_OTG_HS=y` | 兩者相反；`p4-usb.md` 的實測是 HS |
| ESP32-P4-ETH | `features.usb = false` | 沒有關 OTG | build 仍然有 TinyUSB |
| ESP32-C3 | `features.usb = false` | 冇 OTG（正確） | 但 USB-Serial-JTAG 其實是 CDC；`false` 的語義係「冇 OTG」還是「冇 USB」？ |
| ESP32-S3-* | `features.usb = true` | 冇講 FS/HS（S3 只有 FS） | 有 OTG ＋ JTAG，兩者共用 PHY，無法用一個 bool 表達 |

`tools/board.py` 以前完全冇驗 `features.usb`；`system.json` 的 allowlist 亦漏了
`usb` 與 `hostname`（已補，假警告由 4 個減到 2 個）。

### P2 — 其他

- `usb.cdc_log` 在 JTAG 路徑是**刻意忽略**的（2026-11-13 確認）：`ph_usb()` 的
  `!NC_HAS_USB` 分支無條件 `nc_log_start()`，因為 USJ 的 log 與 NC4 共用同一條 TX，
  一定要經 arbiter 分流；直接寫 console 會同 NC4 幀交錯。C3 的 seed 已改寫 1（講真話），
  但這個欄位只對 `kind=cdc` 生效。
- host 與 device 冇 chunk 協商：device 只能默默丟 oversized frame。
- `requirements-dev.txt` 缺 `httpx2`，fresh venv 會在收集 `test_server.py` 時失敗
  （已補；補齊後 36 passed / 1 skipped）。
- 文件仍寫「P4 冇 USB」（`failure-policy.md` §4.1、`config-layers.md`），
  與 P4-C6 已枚舉 OTG 的事實相反。
- `ports/_profiles/` 沒有 USB profile；`p4-usb.md` §14 自己都寫了這個 TODO。

---

## 2. 目標模型

### 2.0 決策（2026-11-13）：兩種 USB transport **不合併**

| | `kind=cdc`（OTG／TinyUSB） | `kind=usb_jtag`（USB-Serial-JTAG） |
|---|---|---|
| 驅動 | TinyUSB device stack | USJ 自帶的 CDC-ACM（LL FIFO） |
| 同時提供的介面 | **CDC-ACM ＋ MSC**（一個 composite device） | 只有一條 CDC-ACM |
| 檔案 | CDC FAT；可選把整塊碟交給 host（MSC） | CDC FAT；**冇 MSC** |
| log | 獨立 CDC 管道，`usb.cdc_log` 控制 | 與 NC4 共用同一條 TX（arbiter 分流），`cdc_log` 無效 |
| 出廠預設 | S2／S3／P4 | C3／C5／C6／C61／H2 |

理由很簡單：**路徑真的不同** —— 不同驅動、不同 endpoint 數、不同能力
（S3 可以交碟、C3 唔可以；P4 可以兩條並存、S3 共用 PHY 只能二選一）。
把它們都叫 `CDC`，只會令「邊塊板交得到碟」變成隱藏規則。

所以：

- link 名、`kind`、能力全部**保持分開**，沒有 alias；
- 晶片表（`nc_cfg_chip.c`）只決定「出廠預設寫邊一條」，不改變語義；
- 寫錯 kind（C3 寫 `cdc`、S3 寫 `usb_jtag`）→ 開機 `usb_links_audit()`
  直接 `ESP_LOGE` 指路，不再靜靜唔建立。


### 2.1 先確定硬件事實：USB 是「介面列表」，不是一個 bool

| 晶片 | USB controller | 內部 PHY | 可否同時用 |
|---|---|---|---|
| **C3** | USB-Serial-JTAG | FSLS ×1（GPIO18/19） | 只有一條 |
| **S3** | USB-OTG FS ＋ USB-Serial-JTAG | **共用 FSLS ×1**（GPIO19/20） | **不可以**：同一條 PHY，只有一個 controller 能用；另一個要用 external PHY |
| **P4** | USB-OTG FS ＋ USB-OTG HS ＋ USB-Serial-JTAG | **FSLS ×2**（USJ 24/25、OTG FS 26/27）＋ **UTMI ×1**（HS 49/50） | **可以**：USJ 同一個 OTG 同時行；兩個 OTG controller 是獨立 peripheral，但 TinyUSB 一次只揀一個 port |

S3 的「不可以」有軟件 mux：`sw_usb_phy_sel` 可以把 internal FSLS PHY 指去 OTG 或 USJ，
但**同一時間只有一邊**；要兩邊同時，就要 external PHY。所以 S3 的兩個 interface
是「同一個實體接口、兩個互斥 personality」。

P4 的 USJ 同 OTG FS 各自有一條 FSLS PHY（PHY0／PHY1），
加上 OTG HS 的 UTMI，三條是獨立的。這就是為什麼 P4 可以「JTAG console ＋ OTG CDC」
同時存在，而 S3 不行。

依據：ESP-IDF `docs/en/api-reference/peripherals/usb_device.rst` 的 ESP32-S3 段
明寫兩個 controller「share a single PHY」；P4 的 `usb_serial_jtag_ll_phy_select()`
則把 USJ／USB_WRAP 映射到 FSLS PHY 0 與 1（兩條），OTG HS 另有 UTMI。

> 所以 `board.json` 不能再寫 `features.usb: true` 或 `otg: "hs"` 一個值；
> 要列**這塊板真正接出來的 interface**，以及它們之間有冇 conflict。

### 2.2 板子事實：`board.json` 用 `usb.interfaces[]`

```jsonc
// P4-C6：兩個 USB-C；usb-c-1 = USJ console，usb-c-2 = OTG HS
"usb": {
  "interfaces": [
    {
      "id": "otg",
      "controller": "otg",          // "otg" | "serial_jtag"
      "speed": "hs",                // "fs" | "hs"
      "connector": "usb-c-2",
      "pins": {"dp": 50, "dm": 49},
      "roles": ["data", "msc"],     // data = 可做 CDC；msc = 可交碟
      "primary": true               // 冇 config 時，CDC 預設放這裡
    },
    {
      "id": "jtag",
      "controller": "serial_jtag",
      "speed": "fs",
      "connector": "usb-c-1",
      "pins": {"dp": 25, "dm": 24},
      "roles": ["data"]             // 也可以做第二條 USB link（kind=usb_jtag）
    }
  ],
  "conflicts": []                   // P4：兩條可以同時
}
```

```jsonc
// S3：一個 USB-C，OTG 與 USJ 共用同一對 D+/D-
"usb": {
  "interfaces": [
    {"id": "otg",  "controller": "otg",         "speed": "fs", "connector": "usb-c",
     "pins": {"dp": 20, "dm": 19}, "roles": ["data", "msc"], "primary": true},
    {"id": "jtag", "controller": "serial_jtag", "speed": "fs", "connector": "usb-c",
     "pins": {"dp": 20, "dm": 19}, "roles": ["data"]}
  ],
  "conflicts": [["otg", "jtag"]]    // 同一對 pins／PHY，只能開一個
}
```

```jsonc
// C3：只有 USJ
"usb": {
  "interfaces": [
    {"id": "jtag", "controller": "serial_jtag", "speed": "fs", "connector": "usb-c",
     "pins": {"dp": 19, "dm": 18}, "roles": ["data"], "primary": true}
  ],
  "conflicts": []
}
```

```jsonc
// P4-ETH：冇接任何 USB（或只有 debug pad，不當成 data interface）
"usb": { "interfaces": [], "conflicts": [] }
```

`tools/board.py` 要負責：

1. **check**：
   - `id` 唯一；`controller`／`speed`／`pins` 是該晶片真的有的組合（C3 不可能有 `otg`；P4 的 `hs` 一定走 UTMI）。
   - 最多一個 `primary`；有多個 OTG interface 時，只有 `primary` 那個可以交給 TinyUSB（一個 device stack 一次一個 port）。
   - `roles` 有 `msc` 的 interface 必須是 `otg`，而且板子要有 data FAT。
   - `conflicts` 內的 interface 不可以同時是 `primary`；runtime 只可以開其中一個。
   - 同 `pins`／`connector` 但冇寫 `conflicts` → warning（多數係漏寫）。
2. **sync**：
   - `otg` ＋ `speed=fs` → `usb-otg-fs` profile（`CONFIG_NETCORE_USB_OTG=y`）
   - `otg` ＋ `speed=hs` → `usb-otg-hs` profile（`+ CONFIG_NETCORE_USB_OTG_HS=y`）
   - 冇 `otg` → `usb-none`（`# CONFIG_NETCORE_USB_OTG is not set`）
   - 有 `serial_jtag` interface → 確保 JTAG transport 有編（不再綁 `!NC_HAS_USB`）
3. 舊 `features.usb` 讀到 bool 時給 warning（遷移期），不要靜靜當冇事。

### 2.3 執行期：`system.json` 也是 `usb.interfaces[]`

```jsonc
// P4：OTG 做 CDC（kind=cdc）＋log；USJ 做第二條 link（usb_jtag，kind=usb_jtag）
"usb": {
  "interfaces": [
    {"id": "otg",  "link": "CDC",  "log": 1, "msc": 0},
    {"id": "jtag", "link": "usb_jtag", "log": 0}
  ]
}
```

```jsonc
// S3：二選一。想 CDC 優先、唔要隨身碟 → 只開 jtag
"usb": {
  "interfaces": [
    {"id": "jtag", "link": "CDC", "log": 1, "msc": 0}
  ]
}
```

```jsonc
// C3：唯一一條，仍然是「CDC」
"usb": {
  "interfaces": [
    {"id": "jtag", "link": "CDC", "log": 1, "msc": 0}
  ]
}
```

規則：

- `id` 對應 `board.json` 的同名 interface；對唔上 → warning，該項忽略。
- `link` 是 `nc4.links[]` 裡的名字（OTG → `CDC`；USJ → `usb_jtag`）；冇寫 = 這條 interface 不做 NC4。
  `nc4.links[]` 仍然是 rx.region／chunk 的唯一來源，USB 這邊只負責「邊條實體接口載住它」。
- `log` 全機只可以有一條 interface 開；第二條要回 warning 並忽略。
- `msc` 全機只可以有一條，而且該 interface 的 `roles` 要有 `msc`；否則 clamp 0。
- **預設**（`usb` 段完全缺失）：`primary` interface 開 `link: "CDC"`、`log: 1`、`msc: 0`；
  其他 interface 關。這樣 C3／S3／P4 都自動有一條 `CDC` 基線。
- **conflict**：同一個 conflict group 只可以開一個 interface。Resolver 揀 `primary`，
  另一個跳過並在 STATUS 記錄原因；**唔可以兩個都開**（S3 會死）。

> `CDC` 只代表 OTG；USJ 是另一條 link（`usb_jtag`，kind=usb_jtag），**不是 alias**。
> 見 §2.0 的決策與能力表。

### 2.4 固件：interface registry ＋ 每條 interface 一個 transport

- 新增一個小 registry：

```c
typedef struct {
    const char *id;              // "otg" / "jtag"
    nc_usb_ctrl_t ctrl;          // OTG / SERIAL_JTAG
    nc_usb_speed_t speed;        // FS / HS
    bool enabled;                // 由 policy resolve
    const nc4_bus_cfg_t *bus;    // 對應 nc4.links[] 的 link
} usb_if_t;
```

- 每個 enabled interface 有自己的 read／send／tx_text；OTG 用 TinyUSB CDC，
  JTAG 用 LL FIFO。唔再是「一條 CDC，transport 換來換去」——
  P4 可以同時有 `CDC`（OTG）同 `usb_jtag`（USJ），兩條獨立 bus、兩種 driver。
- **S3**：兩個 interface 共用 FSLS PHY，編譯期兩邊都編，boot 時只 init 一個；
  要換 personality 就 reboot（software mux 可以切，但 TinyUSB／USJ 要重新 init，唔係 hot-swap）。
- **P4**：USJ 同 OTG 可以同時 init；TinyUSB 只揀 `primary` 那個 OTG port。
- `STATUS_GET keys=sys` 的 `usb` 段改成列表：

```json
"usb": {
  "interfaces": [
    {"id":"otg", "cap":{"ctrl":"otg","speed":"hs","msc":true},
     "active":{"link":"CDC","log":1,"msc":0}, "max_chunk":4096},
    {"id":"jtag","cap":{"ctrl":"serial_jtag","speed":"fs"},
     "active":{"link":"usb_jtag","log":0}, "max_chunk":4096}
  ]
}
```

- `max_chunk` **每條 link 分開報**（OTG 與 JTAG 的 region 可以不同），
  由實際 rx region 反推；host 用它做 chunk 協商（§2.5）。

### 2.5 chunk 是協商結果，不是常數

Device 側：

- `region` 由「想支援的最大 chunk」反推：`region >= chunk + 19`。
- 回報 `max_chunk` 不要用 `region - 19`（會出現 4,077 這種怪數），用階梯
  `8192 / 4096 / 3072 / 2048 / 1024`，揀 region 容得落的最大一級。
- `STATUS_GET keys=sys` 的 `usb.interfaces[].max_chunk` 要**逐條 interface 報**：
  OTG CDC 與 JTAG 的 region 可以不同。
- `FILE_BEGIN` 收到 `chunk > max_chunk` 要**即時拒絕**（明碼 error），
  不可以「收咗先靜靜丟」——後者正是今次 C3 問題難查的原因。

Host 側：

- `preferred = 4096`（可以 config）；問不到 device → 用 **2048** 保守值。
- `chunk = min(preferred, max_chunk)`。
- 如果 ACK timeout／`err_write_fail`：由**最後 ACK 的 offset** 開始，
  chunk 減半重試（下限 512），不重開 session、不重傳已 ACK 的部分。
- 3 KB 一樣合法；4 KB 只是「大多數板食得到」的 preferred，不是協議常數。

為什麼不索性把 region 一律拉到 8,192：

- region 是 `nc_plan` 的一部分（internal RAM），要為最壞情況埋單；
- C3 冇 PSRAM；4,128 已經覆蓋 4 KB 檔案的絕大多數場景；
- 把「上限」與「今次用幾大」分開：RAM 由板子決定、速度由 host 決定。

### 2.6 開機預設：CDC 在前，MSC 在後

建議新板／新 seed 的 `primary` interface 一律：

```json
{"id": "otg", "link": "CDC", "log": 1, "msc": 0}
```

S3 舊用戶如果想要開機見到 `D:`，就把 `msc` 改 1。
這樣「隨身碟」是一個明確選擇，而不是所有板子的預設副作用；
P4-ETH 這一類冇 host 的板子也不會再失去 `/data`。

---

## 3. 分階段

### Phase 0（本次已做）

- C3／JTAG 的 rx region 抬到 4,192（**止血**；§0.5 起改為 4,128）；
  正解是 §2.5 的協商）。
- `tools/board.py` 的 `system.json` allowlist 補 `usb` / `hostname`。
- `requirements-dev.txt` 補 `httpx2`。
- C3 `system.json` 的註解更正（它其實有 CDC，只是走 JTAG）。

驗證：ESP32-C3 建置通過；CDC 工作臺測試 36 passed / 1 skipped。

### Phase 1：板子事實與 build profile

- `board.json` 的 `features.usb` 換成 `usb.interfaces[]`（5 塊板）。
- 新增 `ports/_profiles/usb-otg-fs.defaults`、`usb-otg-hs.defaults`、`usb-none.defaults`。
- `board.py` 的 `check` / `sync` 接上 USB：interface 的 controller/speed/pins 合法、
  最多一個 `primary`、conflict 不可同時開、舊 bool 給 warning。
- P4-C6 究竟是 HS（pins 49/50）還是 FS（26/27）由實板確認；P4-ETH 明確關 OTG。

驗收：`board.py check` 對 5 塊板零 USB 錯誤；P4-ETH 的 build 冇 TinyUSB；
S3 的 `conflicts` 有擋住兩個 interface 同時開。

### Phase 2：執行期 schema 與 interface registry

- `system.json` 的 `usb` 改成 `usb.interfaces[]`（`id` / `link` / `log` / `msc`）。
- `nc_cfg_types.h` 加 interface policy struct；舊 `usb.msc` / `usb.cdc_log`
  當 `primary` interface 的 alias（遷移期；只限 OTG 兩個開關，不影響 kind 分家）。
- 固件加 interface registry；OTG 用 TinyUSB、JTAG 用 LL；P4 可以兩條同時。
- S3：boot 時由 policy 選 OTG 或 JTAG（software mux），只 init 一個。
- `STATUS_GET` 的 `usb` 改成 interface 列表（cap / active / 每條 `max_chunk`）。
- host 讀 `max_chunk`：未知時 2048，`chunk = min(preferred, max_chunk)`；
  timeout 由最後 ACK 減半重試。
- `FILE_BEGIN` 對超額 chunk 明碼拒絕。

驗收：C3／S3／P4 用同一份 `system.json` 形狀；P4 可以同時有 `CDC`（OTG）
與 `usb_jtag`（USJ）；S3 切換 personality 只需改 config + reboot；
同一個 host 對 2 KB／3 KB／4 KB 上限的 device 都上傳成功。

### Phase 3：驗證與文件

- C3：2 KB／3 KB／4 KB 上傳、下載、manifest（真機）；強制細 region 時要自動降級。
- S3：CDC FAT、`msc=0` 與 `msc=1` 兩種模式；OTG 與 JTAG personality 各一次。
- P4-C6：OTG HS 的 `CDC`；USJ 的 `usb_jtag` 同時存在；兩條 link 各自 route、各自 driver。
- P4-ETH：開機後 `/data` 仍然在 APP（唔會被交出去）。
- 更新 `failure-policy.md` §4、`config-layers.md`、`p4-usb.md` §14 的舊結論。

---

## 4. 一句話總結

現在 USB 的「有冇」由 target 決定、「點用」由兩個 runtime 布林決定、
「板子實際接咗乜」寫在 `board.json` 但冇人讀 —— 三邊各自為政。
把 `board.json` 的 USB 能力變成 **interface 列表**（controller／speed／pins／connector／conflict），
runtime 也用同一個列表配對，再讓每條 interface 各自載一條 link，
「所有型號第一步都有 CDC（NC4）」就由口號變成 `board.py check` 擋得住的規則；
P4 的兩個 USB-C 也自然變成兩條獨立 link，而不是一個 `transport` 欄位。
