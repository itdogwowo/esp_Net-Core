# 全樹技術進化日誌（NC4／通道／緩衝／效果／pixel）

> **為什麼有這份文件**：`pixel-evolution.md` 只記 pixel 管線。但這棵樹的優化
> 唔止一條線：NC4 熱路徑、通道吞吐、緩衝貼身化、效果層、SD／USB、開機。
> 每一條都係「量到問題 → 改 → 再量」，數字散在 30 個 commit message 同
> 十幾節 status。呢份係**全樹的時間線**：一條主線一節，每步有 commit、
> 有前後數字、有量測點。
>
> - **性質**：歷史紀錄；新一條 append，唔覆蓋舊條目。
> - **快照**（現況數字）仍然以 [status.md](../00_pm/status.md) 為準。
> - **Pixel 管線細節**另有專文：[pixel-evolution.md](pixel-evolution.md)
>   （編碼 143 ms → 6.36 ms 的逐階段、失敗實驗、8×8 SWAR 圖解）。呢份只放摘要＋連結。
> - 量測方法：`docs/02_guides/measurement-method.md`；物理上限／達標率原則見 §12。
> - 最後更新：2026-11-26（首次建立；涵蓋 9 條主線）。
>
> **唔包**：非效能類的架構演進（任務模型、config 四層、driver 分層、命名）。
> 那些看 [task-model.md](../03_notes/task-model.md)、[config-layers.md](../03_notes/config-layers.md)、
> [driver-layer.md](../03_notes/driver-layer.md)。

---

## 0. 主線總覽

| # | 主線 | 起點 → 現在 | 最大倍數 | 詳細 |
|---|---|---|---:|---|
| A | NC4 收發核心（decoder／插槽／熱路徑） | 共用鎖 → 每核一個；hub 102k → 257k ops/s | 2.5× | §2 |
| B | 通道與網絡（P4／S3） | WS RTT P4 6.5 → 3.6 ms；S3 14–16 → 7.4 ms | 2× | §3 |
| C | 緩衝與記憶體 | UART −13.3 KB、棧 −6.4 KB、靜態 −3,136 B、`.bss` −65 KB | 資源 | §4 |
| D | 任務／啟動／棧 | 靠 stack_cover 實測精確配棧；爆棧事件修 | 資源 | §5 |
| E | 效果層（evaluate／gamma） | gen 15.6 → 2.0 ms；full8 16.57 → 4.41 ms | 7.8× | §6 |
| F | 配對層（layout） | 零拷貝 9.86 → 3.27 ms；6 B 表 2.13×；隨機 3.97× | 3.0× | §7 |
| G | 編碼／輸出（I80） | 16×600 24.0 → 6.36 ms | 3.77× | §8／專文 |
| H | USB／FAT／SD／OTA | SD 4K 2,124 → 64K 8,460 KB/s（92% 滿速） | 4.0× | §9 |
| I | 建置／版本／工具 | IDF 統一；靜態帳可重現；bench 家族 | — | §10 |

> 「最大倍數」係該主線最好的一個節點，唔係整體；每節逐條列。

---

## 1. 讀法與名詞

| 名詞 | 意思 |
|---|---|
| **達標率** | 實測 ÷ 物理上限（例：UART 90 KB/s ÷ 921600 baud 98%；SD 8,460 ÷ 9,152 = 92%） |
| **HWM** | High Water Mark：任務棧用過的最高水位（`stack_cover` 量） |
| **貼身化** | 按實測 HWM／峰值把 buffer 縮到剛好，唔靠估 |
| **零拷貝** | 資料只寫一次到「下一個消費者會讀的位置」，中間唔複製 |
| **gather／broadcast** | 見 pixel-evolution §0（編碼用） |

---

## 2. 主線 A：NC4 收發核心

| 日期 | commit | 改動 | 之前 → 之後 | 量測點 |
|---|---|---|---|---|
| 11-04 | `1779cf1` | 未啟用的通道**唔宣告緩衝**（`links[].enable` 唯一真相）；WS／UDP 暫存移 PSRAM | S3 DIRAM **107,721 → 97,929 B**（−9,792）；P4 129,023 B | 建置 size |
| 11-08 | `37cdefa` | **每核一個 decoder**：`s_decoder[2]`，各自執行鎖／回覆幀（2,615 B PSRAM）／位址狀態；共用唯讀指令表；OTA 加一把 mutex | 一條 CDC 指令唔再鎖死另一核的 UART 指令 | 0xF00F `UART_INJECT` 驗「一核卡住、另一核照跑」 |
| 11-08 | `7f2e480` | 測試回覆幀由 by-value 2,615 B struct 改 `nc4_decoder_reply_buf()`（PSRAM） | 單條 `0xF00C` 的 decode0 HWM **2,580 → 364**；4 KB 洪水原本只剩 124 B，修後整體 HWM 2,476 | 上板 HWM |
| 11-08 | `7f2e480` | 儀表：`nc4_parser_peaks()`（max_dlen／max_ok）、`nc4_bus` reads／max_read／slot_full／tx | 之後所有 buffer 尺寸都有實測依據 | `tools/bench/buf_probe.py` |
| 11-08 | `aaac713` | **靜態分區收斂**：`NC_PLAN_MAX_DECLS` 64→16（−1,920 B）、`NC4_MAX_BUSES` 8→4（−1,216 B） | 開機內部 heap **287,383 → 290,519**（正好 +3,136） | `status mem` |
| 11-09 | `dd46be4` | `nc_hub` 熱路徑：state 改 32-bit atomic（S32C1I）、核心檢查只做 load、移除 lease 時間戳 | 單核位址交換 **102k → 257k ops/s**；`0xF003` HUB_BENCH **81k → 126k ops/s** | 0xF01F–0xF024／`0xF003` |
| 11-10 | `eb7c1fd` | `nc_buf` 統一全部通道（FIFO／STREAM）；刪 `nc_pool`（零用戶但佔 28.3 KB internal）；decode 直接由插槽解析 | internal **+32.8 KB**；每幀 copied **36 → 18 B** | `status mem` |
| 11-14 | `b6d63a5` | `nc_buf` RING 的 SPSC counter ownership 修正；C11 atomics 做成 opt-in | host 2-thread repro **2,226 mismatches → 0**；atomics 版 ring microbench **−0.6%**（所以預設唔開） | host repro + 上板 microbench |

**這條線的教訓**：三個「看起來正常、其實燒資源」的問題（UART 等收滿才返回、
2,615 B by-value 回覆、環尺寸照抄放大）全部係**量了才看到**；修完吞吐冇退
（UART 90 KB/s ＝ 921600 baud 的 98%、CDC RTT 1.07–1.13 ms、CDC TX 1 MB 1.024 s）。

---

## 3. 主線 B：通道與網絡

| 日期 | commit／文件 | 改動 | 之前 → 之後 | 量測點 |
|---|---|---|---|---|
| 11-15 | `net-results-2026-11-15.md` §2 | WS 小回覆：`TCP_NODELAY` ＋ single-send ＋ 32-bit unmask | WS RTT **P4 6.5 → 3.6 ms**、**S3 14–16 → 7.4 ms**（貼近 TCP 的 3.5–4.0／7.4–8.7） | request/reply |
| 11-15 | 同上 §3 | WS 細包改「一個 WS message 裝多個 NC4 frame」（batch 10）；slot 要 16,384 | 1 KB：batch1 **1.87 → 5.51 MB/s**（TCP 5.51）；WS＝TCP 的 98–105% | 單向 NC4 |
| 11-15 | 同上 §4 | P4 raw TCP 單 stream（source SRAM 4K、SO_SNDBUF 32 KB、core1 prio12） | PC→device **8.15–8.83**；device→PC **7.94–8.19** | raw socket |
| 11-15 | 同上 §5 | P4 raw TCP 雙 stream（每核一條） | **9.221／9.545 MB/s**（單 stream 8.2–8.5 → **+8–16%**）；download 已超舊 MPY 9.35 | raw socket |
| 11-15 | 同上 §3 | 認清「NC4 協議成本」 | raw 8.5 → NC4 **5.5 MB/s**（parser／handler 成本，唔係網卡） | 對照 |
| 11-08 | `db1d850` | 方法：**用物理上限做參考線**（TCP 視窗÷RTT、PHY、baud），工具回 wire bytes/s | 之後每個數字都有「達標率」 | 方法 |

**未解／界線**：S3 WiFi 上限 ~0.8 MB/s（C 版仍未穩定超過 MPY）；P4 device→PC
單 stream 仍低 MPY 12–14%（雙 stream 已補回）。

---

## 4. 主線 C：緩衝與記憶體（貼身化）

| 日期 | commit | 改動 | 之前 → 之後 | 驗證 |
|---|---|---|---|---|
| 11-04 | `1779cf1` | 通道未啟用 = 0 B（見 §2） | S3 DIRAM −9,792 B | 建置 size |
| 11-08 | `7f2e480` | **UART 環照實測縮**：slot 4×1,024→4×256、RX 4,096→1,024、TX 8,192→1,024 | 共 **−13.3 KB** | `buf_probe` max_read 246 B、slot 峰值 1 |
| 11-08 | `7f2e480` | **棧按 HWM 縮**：uart.rx 4,096→1,536（實測 888）、cdc.rx 1,536→1,280（936）、decode0/1 4,096→3,072（走遍 handler 2,232）、main 4,096→3,072（2,120）、nc_log 3,072→2,560（1,804） | 共 **−6.4 KB** | `stack_cover` |
| 11-08 | `0ea85fa` | UART RX 環 1 KB vs 2 KB **對照實驗** | 1 KB：3 次 crc_err／4,135 循環；2 KB：1 次／1,620 → **同數量級，加大環冇用**，維持 1 KB | 30 分鐘 soak |
| 11-08 | `aaac713`／`6ebf173` | UART 偶發 CRC 真兇＝**同核同級競爭**（fx 測試任務 prio 3 同 uart.rx 同級） | fx 降優先級 → 事件率 **−3.5×** | soak7 |
| 11-08 | `7f2e480` | 64 KB SD bench buffer 由 `.bss` 改按需配置（internal DMA） | `.dram0.bss` **109,360 → 44,096 B**；開機內部剩 **211,623 → 276,959**；最大連續 **139,264 → 204,800** | 建置 + `status` |
| 11-10 | `eb7c1fd` | 刪 `nc_pool`（零用戶） | internal **+32.8 KB** | `status mem` |

**「放哪裡」的實測結論（唔係信仰）**：

| 情境 | internal | PSRAM | 結論 |
|---|---:|---:|---|
| SD 寫 2 MB（S3） | 9,152 KB/s | **3,271 KB/s** | SD buffer 一律 internal；PSRAM 慢 2–3×（S3 SDMMC DMA 到唔到 PSRAM） |
| 效果值流 38,400 值 | 1.13 ms | 5.31 ms | 仍然選 PSRAM（容量政策；26.5% 預算可接受） |
| 配對 6 B 表 | 2,185 µs | 4,812–5,155 µs | 只准 internal（PSRAM 版慢過 runs） |

---

## 5. 主線 D：任務／啟動／棧

| 日期 | commit | 改動 | 效果 |
|---|---|---|---|
| 11-08 | `7f2e480` | 所有棧按 `stack_cover` HWM 重配 | 見 §4；decode 剩 840 B 安全邊際 |
| 11-10 | `eb7c1fd` | 優先級保留區 7–15（非 SYSTEM 自動夾到 6 並警告）＋ loop 心跳 | 測試任務唔會再靜靜搶同級（CRC 事件根因） |
| 11-24 | `197e1e0` | `0xF077` 回覆 JSON 由 stack `j[1200]` 改 `static` | frame **2,272 → 1,168 B**（decode0 棧 3,072）；任何 variant 唔再 panic |

**開機基線**（`docs/03_notes/naming.md`）：`B01 9.66 ms｜B02 126.92 ms`
（B02 ＝ 掛 FAT ＋ 讀 config）。**呢個只係基線，未做過優化**——要壓 B02
就要改 storage 掛載／config 解析，屬未做項。

---

## 6. 主線 E：效果層（program → 值流）

| 日期 | commit | 改動 | 之前 → 之後 | 量測點 |
|---|---|---|---|---|
| 11-22 | `7dd526e` | 認清 12-bit vs 8-bit canvas：**12-bit canvas 落咗 PSRAM**，random RMW 係主因 | 9,600 RGB random：12-bit **19.967 ms** → 8-bit **0.970 ms**（**20.6×**）；packed12 20.600 ms 證明唔係「store 次數」 | EffectPerf probe |
| 11-22 | `06774f2` | **wave table 預算**：program 只有 256／4,096 長，播放時把值先算成 `uint16_t wave[]`，每幀只做 index | gen 28,800 值 **15.603 → 1.993 ms**（7.8×）；64 px effect avg **46 → 7 µs**；9,600 RGB full8 **16.572 → 4.408 ms**（50 fps 的 82.9% → **22.0%**） | 上板 probe |
| 11-24 | `9a4d283` | 效果層 buffer **一律 PSRAM**（政策）；只有細表（正弦 514 B、波表 512 B）留 internal | 9,600 值 0.464 ms（48.3 ns/值）、28,800 3.98 ms、38,400 5.31 ms（**26.5%** 預算）；建表 113 µs／256 值 | `0xF065` |
| 11-24 | `cdba7b4` | gamma 成本量清（8-bit 引擎專用；12-bit 走 `>>4` 唔經） | gamma **151 ns/值** vs `>>8` 16.8 ns；9,600×4 值＝**5.80 vs 0.64 ms** | `0xF065` `gam_ns` |
| 11-24 | `5de9e59` | gamma 影響可視化（golden test） | 同一輸入：冇 gamma 中間 **128**、有 gamma **55**（21%）；一條 fade 要 **73%** 時間才看起一半亮 | host `gamma_bench` |
| 11-24 | `e7c8d27`＋`8837a59` | aprog 按實際 distinct 效果配置（移除固定 32） | 40 個效果真機驗證：jobs 40／naprog 40、fps 49.9、pair apply 4.36 ms | `0xF079`／`0xF061` |

**取值**：如果只記一件事——**「一次預算、之後重用」**（wave table）係效果層最大槓桿；
而「放哪里」比「演算法」更貴（12-bit canvas 20× 就是例子）。

---

## 7. 主線 F：配對層（layout）— 摘要

| 日期 | commit | 改動 | 數字 |
|---|---|---|---|
| 11-24 | `e13d673` | 零拷貝寫環：`begin → apply → commit`，刪 canvas／每幀 memset／畫布→環複製 | 9,600 px 每幀 **9.86 → 3.27 ms（−67%）**；拆開：clear 2.14＋write 3.15＋copy 4.57 → write 3.26＋commit 0.01 |
| 11-24 | `540ef90`／`c5c2ddb` | 6 B 單表（dst16／src16／write8／aprog8）＋**只准 internal** | 500 組 3,745 → **2,954 µs**；1000 組 2 段 4,645 → **2,185 µs（2.13×）**；放 PSRAM 反而 **0.60×** |
| 11-24 | `4e8b468`／`a3fe75c`／`4ec8a27` | 極端分佈量測（完全隨機／每粒一 group／通道級） | 9,598 段 **11,950 → 3,007 µs（3.97×）**；9,600 job **19,025 → 3,009（6.32×）**；通道級 **12,144 → 1,589（7.64×）** |
| 11-25 | `c9d595f`／`d965934` | flat 一律 PSRAM；playlist 預建下一張＋swap | flatify 550 µs；apply 3.07（internal）／5.37（PSRAM）ms；**swap 246 µs**（原本 rebuild 40–150 ms） |

**完整逐條（含負結果）見 [pixel-evolution.md §2](pixel-evolution.md#L204)。**

---

## 8. 主線 G：編碼／輸出（I80）— 摘要

| 階段 | 改動 | 16×600 |
|---|---|---:|
| 起點 | 逐 timing unit shift／OR | 143 ms |
| 11-19 `467af43` | 波形 LUT＋packed bitstream＋nibble spread＋PSRAM DMA 槽 | 14.33 ms |
| 11-24 `197e1e0`＋`dd64ef8` | `pack_bus` 一次過展開＋轉置；8-bit／無抖動廣播 | **6.36 ms（3.77×）** |

失敗實驗：只換 8×8 SWAR 轉置 = **1.22×**（瓶頸係 gather，唔係算術）。
**完整逐條（含 1-lane 回歸、量測點警告）見 [pixel-evolution.md](pixel-evolution.md)。**

---

## 9. 主線 H：USB／FAT／SD／OTA

| 日期 | 改動／量測 | 數字 |
|---|---|---|
| — | OTA（NC4 0x22xx，4 KB chunk、stop-and-wait） | 388,704 B／2.0–2.7 s ＝ **142–191 KB/s**；400,992 B／2.8 s ＝ 140.4 KB/s；典型 **150 KB/s** |
| 11-19 `467af43` | OTA `--window` pipelining（工具側） | 減少 stop-and-wait 空檔（工具行為，唔改裝置協議） |
| — | SD FAT **區塊大小**實驗（源頭 128 KB PSRAM、64 KB 內部 staging） | 4 KB **2,124 KB/s（23%）** → 16 KB 4,216（46%）→ 32 KB 5,970（65%）→ **64 KB 8,460（92%）**；全程內部對照 9,152 |
| — | 同一個實驗的 memcpy 成本 | PSRAM→internal 2 MB 固定 **~39 ms（≈52 MB/s）**，佔比 4%（4 KB）→16%（64 KB） |
| — | MSC（碟交 USB host）寫入 | 4–6.3 MB / 65–76 KB/s（基線，未優化） |

**結論（設計原則）**：SD／FAT 快慢的槓桿係**每次寫入的 chunk 大小**，唔係「有冇 batch」；
chunk ≥32 KB 才有意義，64 KB 才接近滿速。

---

## 10. 主線 I：建置／版本／工具

| 項目 | 內容 |
|---|---|
| IDF 版本 | macOS／Windows `.idf`／WSL **統一 v6.1.1**；統一前實測同一份 code：Matrix image 差 **+9,584 B**、P4-ETH **+24,688 B**（所以唔可以跨版比數字） |
| 影像大小 | 測試指令拆出 `action/test_action`：tests OFF 時 handler 唔入映像，**S3 −6,512 B／P4 −6,800 B**；靜態分區收斂 −3,136 B |
| Host 測試 | `run_all.sh` **21 支**（另有 4 支係 bench，唔入 run_all） |
| 量測工具 | `buf_probe.py`（緩衝／棧／RTT）、`layout_bench.py`（配對／I80）、`effect_bench.py`（`0xF065`）、`pipe_bench.py`（效果管線）、`fps_trace`／`fps_drift`／`fps_longrun` |
| 一次性 lab | `modules/lab/i80_pack_lab/`（Task 6 A/B）、各 `*_bench` host 程式 |

---

## 11. 跨主線負結果（唔好再試一次）

| 實驗 | 量到 | 結論／出處 |
|---|---|---|
| UART RX 環 1 KB → 2 KB | crc_err 3 次/4,135 vs 1 次/1,620 | 同數量級，唔關容量（`0ea85fa`） |
| C11 atomics RING | ring microbench **−0.6%** | 預設唔開（`b6d63a5`） |
| PSRAM tiling | 快 **1.3×** | 唔夠；PSRAM 20–33 MB/s 就係上限（`5616eae`） |
| 12-bit canvas 全放 PSRAM | random 寫 **19.967 vs 0.970 ms** | 20× 慢；保持 8-bit canvas（`06774f2`） |
| 值流放 internal | 38,400 值 **1.13 vs 5.31 ms** | 快，但政策選 PSRAM（容量） |
| 只換 SWAR 轉置 | **1.22×** | 瓶頸係 gather（`§7.201`） |
| 1-lane 逐 tick 廣播 | 840 µs vs 舊 357 µs | 1 lane 走 LUT memcpy（`dd64ef8`） |
| 4 B 表入 production | bench 2.06×／5.45× | 接線後板 USB 消失 → 已回退（`325511e`） |
| flat chunk 預取 | bench 1.6 vs 5.0 ms；production 冇增益 | Kconfig 預設關（`49a788c`） |
| RGB 專用／pipe core／值流 internal | 全部無增益（5,005–5,422 µs） | 保留無害改動，其餘回退（`5f324bc`） |
| 4 byte store → 1 次 32-bit store | 2,954 → 4,393 µs（慢 1.5×） | store buffer 併 cache line 更好（`c5c2ddb`） |
| 加大 slack cap 解 overrun | 政策禁止 | 要改效果或降 FPS（AGENTS §10.5b） |

---

## 12. 量測方法（呢棵樹的規矩）

1. **同一塊板、同一映像、同一 workload**；A/B 要同 payload 同 buffer。
2. **正確性閘門先過**：逐 byte `memcmp`／checksum／`bus_ok=1`，出錯就唔睇時間。
3. **取 min（或 median）＋寫 reps**；reps 8 同 16 要一致（唔一致＝有抖動源）。
4. **用物理上限做參考線**：UART 90 KB/s ÷ 921600 baud ＝ 98%；SD 8,460 ÷ 9,152 ＝ 92%；
   16×600 線路 18 ms；TCP 視窗÷RTT。冇參考線的「快咗 3 倍」冇意義。
5. **標量測點**：`0xF062`（bench）／`0xF077`（EffectPerf）／production `enc_us`／
   host x86 係四個世界，**唔可以跨表比**。
6. **host 數字唔可以外推**（x86 2.72× ≠ S3 3.77×）。
7. **負結果要寫低**（見 §11）。

---

## 13. 未完成

| 項目 | 狀態 |
|---|---|
| 16-lane × 16-bit I80 實機驗證 | 冇板；encode 6.36 ms < 18 ms 線路 |
| B02 開機 126.92 ms 優化 | 未做（storage 掛載＋config 解析） |
| S3 WiFi > 0.8 MB/s | 未做到；C 版仍未穩定超 MPY |
| P4 device→PC 單 stream（仍低 MPY 12–14%） | 雙 stream 已補回；單 stream 未解 |
| 12-bit canvas 留在 internal（tile／chunk） | 未做；現時 8-bit canvas 定案 |
| RMT（C 系列）／PARLIO（P4）後端 | 未實作（[pixel-output-backends.md](../03_notes/pixel-output-backends.md)） |
| 4 B 表入 production | 回退；要先 lab 完整 boot 驗證 |

---

## 14. Commit 索引（時間序）

| 日期 | commit | 一句話 |
|---|---|---|
| 11-04 | `1779cf1` | 未啟用通道 0 B；WiFi 兩道閘；S3 DIRAM −9,792 B |
| 11-08 | `37cdefa` | 每核一個 decoder |
| 11-08 | `7f2e480` | 緩衝貼身化：UART −13.3 KB、棧 −6.4 KB、儀表 |
| 11-08 | `0ea85fa` | UART RX 1 KB vs 2 KB 對照（維持 1 KB） |
| 11-08 | `aaac713` | 靜態分區 −3,136 B；CRC 真兇（同核同級競爭） |
| 11-08 | `6ebf173` | soak7：fx 降優先級 → CRC 事件 −3.5× |
| 11-08 | `db1d850` | 物理上限當參考線（方法） |
| 11-09 | `dd46be4` | nc_hub 熱路徑：102k → 257k ops/s |
| 11-10 | `eb7c1fd` | nc_buf 統一；刪 nc_pool（+32.8 KB）；copied 36 → 18 B |
| 11-14 | `b6d63a5` | RING SPSC 修正（2,226 → 0）；atomics −0.6% 唔開 |
| 11-15 | （文件）`net-results-2026-11-15.md` | WS RTT P4 6.5→3.6、S3 14–16→7.4；雙 stream 9.22／9.55 |
| 11-19 | `467af43` | I80 編碼 35.4 → 14.33 ms |
| 11-22 | `7dd526e` | 8-bit canvas：12-bit random 19.967 → 0.970 ms（20.6×） |
| 11-22 | `06774f2` | wave table：gen 15.603 → 1.993 ms；full8 16.572 → 4.408 ms |
| 11-24 | `e13d673` | 配對零拷貝：9.86 → 3.27 ms |
| 11-24 | `540ef90`／`c5c2ddb` | 6 B 表 internal：2.13× |
| 11-24 | `4e8b468`／`a3fe75c`／`4ec8a27` | 極端分佈 3.97×／6.32×／7.64× |
| 11-24 | `9a4d283` | 效果緩衝一律 PSRAM（38,400 值 5.31 ms） |
| 11-24 | `cdba7b4`／`5de9e59` | gamma 151 ns/值；可視化 golden |
| 11-24 | `e7c8d27` | aprog 按實際數量配置 |
| 11-24 | `197e1e0` | pack_bus 原型；`0xF077` 爆棧修 |
| 11-24 | `dd64ef8` | 編碼接 production：24.0 → 6.36 ms（3.77×） |
| 11-25 | `c9d595f`／`d965934` | flat 一律 PSRAM；swap 246 µs |
| 11-25 | `8837a59` | 去固定 32：40 效果 |
| 11-25 | `49a788c`／`5f324bc`／`325511e` | 三組負結果＋4 B 表事故 |
| 11-26 | `8e4230f` | pixel-evolution 專文（本份的 pixel 章節） |

---

## 15. 維護規則

1. **新的一條 append**，編號接落去（A10、B7…），唔好插入中間改歷史。
2. 每條要有：**日期、commit、改了什麼、之前 → 之後、量測點、正確性閘門**。
   冇數字就標「未量」，唔好寫「應該快咗」。
3. **失敗／無效的實驗一樣要寫**（§11）：寫「假設 → 實測 → 為什麼錯」。
4. 量測點不同（bench／production／host／別塊板）**唔可以跨表比**。
5. 呢份係**歷史**；現況快照去 [status.md](../00_pm/status.md)，
   pixel 細節去 [pixel-evolution.md](pixel-evolution.md)，
   設計理由去各 subsystem 的 `03_notes`。
