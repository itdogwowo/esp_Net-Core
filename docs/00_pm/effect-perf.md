# 效果效能專用專案（ESP32-S3-Matrix-EffectPerf）

> **量什麼**：效果管線本身 —— program interpreter 逐值／逐幀、8-bit／12-bit 配對、
> 12→8（四捨五入／抖動）、internal SRAM／PSRAM／混合放置、分塊、多 job。
> **不量**：基礎燈效（OFF／SOLID／ZONE1／RAMP／BREATH／WAVE）、encode、送燈、
> network、ncm、其他 `0xF0xx` 測試。
> **最後更新**：2026-11-20（實機 S3 240 MHz、IDF 6.0.3、2 MB PSRAM @ 80 MHz）

---

## 1. 為什麼要另開一個專案

`NC_ENABLE_TESTS=ON` 的映像會把 **58 條測試指令**、NCM（512 KB arena）、
network 測試 hook 全部拉進來；量效果的時候，cache／IRAM／heap 旁邊一定有其他東西。
`ESP32-S3-Matrix-EffectPerf` 把變數收乾淨：

| 項目 | LED-Textkit／tests 映像 | **EffectPerf** |
|---|---|---|
| `CONFIG_NETCORE_EFFECT_BENCH` | n | **y** |
| `NC_ENABLE_TESTS` | y（58 條 `0xF0xx`） | **n** |
| NCM／WAMR arena | y（512 KB PSRAM） | **n** |
| render 任務（I80 送燈） | y | **n**（`tasks.json` level=-1） |
| pixel 任務（基礎燈效） | y | **n**（level=-1） |
| 燈效 | 會亮 | **不亮**（也不初始化 I80） |
| NC4 指令 | 全部 | 只有公共指令＋`0xF065` |
| 內部 heap（開機） | ~102 KB（tests 映像） | **234.7 KB** |

**這就是「不同模組用 PSRAM」的對照組**：EffectPerf 不佔 512 KB arena、不開
render／pixel，內部 SRAM 多出約 **132 KB**；同一塊 9,600 px 的 12-bit 畫布
（76.8 KB）在 EffectPerf **分得到 internal**，在滿載映像就分不到（status §7.147）。
所以「畫布落 PSRAM」很多時候不是畫布太大，而是其他模組先佔了 internal。

---

## 2. 專案與建置

```text
ports/project/S3/ESP32-S3-Matrix-EffectPerf/
├─ project.json                 hardware = S3/ESP32-S3-Matrix；alias effect-perf
├─ sdkconfig.defaults           CONFIG_NETCORE_EFFECT_BENCH=y（不開 NC_ENABLE_TESTS）
└─ data/config/
   ├─ tasks.json                render／pixel = -1；effect_bench = level 4、core1
   ├─ system.json               64 px、USB CDC、WiFi 關
   ├─ network.json / resources.json
   └─ hardware.json             wiring 的鏡子（同 Matrix-LED-Textkit）
```

```bash
# 建置（第一次要 --fresh 套 project defaults）
IDF_PATH=~/esp/esp-idf python3 make.py build ESP32-S3-Matrix \
  --project ESP32-S3-Matrix-EffectPerf --fresh

# OTA（WebMaster 已開；同 UI 共用一條 CDC）
curl -X POST 'http://127.0.0.1:8011/api/firmware?apply=1&expect_chip=esp32s3' \
     -H 'Content-Type: application/octet-stream' \
     --data-binary @artifacts/ESP32-S3-Matrix__ESP32-S3-Matrix-EffectPerf.bin

# 量測（預設 64；可一次過 64,960,9600）
python3 tools/effect_bench.py --via http://127.0.0.1:8011 --px 64
```

`effect_bench` 開機 **8 秒後自動跑一次 64 px／2,000 幀**（約 6 秒），
log 直接出在 WebMaster 終端；要更穩的數字用 `0xF065` 指定 frames。

### 2.1 為什麼不是 `test_*`

`effect_bench` 是 `CONFIG_NETCORE_EFFECT_BENCH` 的獨立 component
（`firmware/components/tasks/effect_bench/`），**不是** `components/*/test_*`。
`NC_ENABLE_TESTS=OFF` 時它仍然可以編進來；`CONFIG` 關閉時整段 `.c` 是空的，
不登記任務／指令。這滿足「不需要其他測試參與」。

---

## 3. `0xF065 EFBENCH`

payload `[px:u16 LE][frames:u16 LE]`（frames=0 = 按 px 自動：64→10000、
960→500、4096→100、9600→20；上限 `1,000,000/px` 幀，避免單次 `fn()` 超過 5 s WDT）；
回覆 `0xF066`，payload = 2 B 長度前綴 ＋ JSON。

```json
{"px":64,"fr":1000,"rep":3,"nv":64,
 "mem":{"c8i":1,"c8p":1,"c12i":1,"c12p":1,"ifree":234687,"pfree":2085480},
 "eval_ns":480,"vals_ns":33753,
 "pair8":{"i":17160,"p":17160},"pair12":{"i":16055,"p":16055},
 "conv":{"i":14080,"p":14063,"di":23732,"dp":23716},
 "pipe":{"i8":50857,"p8":50857,"i12":63807,"p12":63822,"mix":63791},
 "chunk":{"64":63866,"256":63866,"1024":63866},
 "jobs":{"j2":75776,"j4":151528}}
```

| 欄位 | 意思 |
|---|---|
| `eval_ns` | `nc_px_engine_program_value()` 每個 12-bit 值（ns） |
| `vals_ns` | `nc_px_job_vals()` 每幀算 `nv` 個值（8×8 的 `nv=64`） |
| `pair8/12` | `nc_px_pair_apply()`／`pair_apply12()` 把值散入畫布（ns/幀） |
| `conv` | `nc_px_canvas12_to_8()`；`i/p`=12-bit 畫布在 internal／PSRAM；`di/dp`=抖動 |
| `pipe.i8/p8` | 整條效果（vals＋pair）→ 8-bit 畫布 internal／PSRAM |
| `pipe.i12/p12` | 整條效果 → 12-bit 畫布 → 12→8；兩塊都 internal／都 PSRAM |
| `pipe.mix` | **12-bit 畫布 PSRAM → 8-bit 畫布 internal**（最現實的混合） |
| `chunk` | 12-bit 畫布切 64／256／1024 px，internal scratch 上做配對＋轉換 |
| `jobs` | 2／4 個 job（mode 委派）合成到同一塊 12-bit 畫布 |

量測法：每組先熱身 1/8，再跑 `rep` 次取最小；`rep` = 3（px ≤ 1024）／2（大 px）。
量測區間外每輪 `vTaskDelay(2)` 餵 task WDT（見 §6）。

---

## 4. 實機數字（S3 240 MHz，EffectPerf 映像，正常 nc4 任務跑住）

### 4.1 8×8（64 px）—— 本次的主角

| 階段 | internal | PSRAM |
|---|---:|---:|
| 效果逐值 `eval` | 480 ns/值 | — |
| 每幀 64 個值 `vals` | **33.75 µs** | — |
| 8-bit 配對 | 17.16 µs | 17.16 µs |
| 12-bit 配對 | 16.05 µs | 16.05 µs |
| 12→8 捨入 | 14.08 µs | 14.06 µs |
| 12→8 抖動 | 23.73 µs | 23.72 µs |
| **整條效果 8-bit** | **50.86 µs** | 50.86 µs |
| **整條效果 12-bit** | **63.81 µs** | 63.82 µs |
| **12p→8i 混合** | — | **63.79 µs** |
| 分塊 64／256／1024 | 63.87／63.87／63.87 µs | — |
| 多 job 2／4 | 75.78／151.53 µs | — |

- 64 px 全 12-bit＋抖動 **63.8 µs/幀** ＝ 50 FPS 預算（20 ms）的 **0.32%**。
- 其中 **33.7 µs（53%）是「算 64 個效果值」**，配對＋轉換只佔 30 µs；
  所以 8×8 要再快，先減 `pixel_hint`／`eval`，唔係換畫布。
- 64 px 的畫布只 256 B（8-bit）／512 B（12-bit），internal 與 PSRAM **完全同速**（cache 吸掉）。

### 4.2 放大到 960／4,096／9,600 px

| px | 整條 12-bit internal | 整條 12-bit PSRAM | 混合（12p→8i） | 分塊 1024 | 12-bit i12 佔 20 ms |
|---:|---:|---:|---:|---:|---:|
| 64 | 63.8 µs | 63.8 µs | 63.8 µs | 63.9 µs | 0.32% |
| 960 | 500.6 µs | 500.6 µs | 500.6 µs | 500.7 µs | 2.5% |
| 4,096 | 2.039 ms | 3.433 ms | 2.039 ms | 2.128 ms | 10.2%（p12 17.2%） |
| **9,600** | **4.738 ms** | **11.095 ms** | **8.701 ms** | **5.005 ms** | **23.7%（p12 55.5%）** |

- **960 px 以內**：12-bit 畫布 7.7 KB，internal／PSRAM 同速；PSRAM 唔係問題。
- **4,096 px**：12-bit 畫布 32 KB 開始超出 cache，`p12` 比 `i12` 慢 **68%**
  （3.433 vs 2.039 ms）；但混合（12p→8i）仍然 2.039 ms，因為 8-bit 輸出在 internal。
- **9,600 px**：12-bit 畫布 76.8 KB，`p12` 11.095 ms（**55.5%** 預算），
  `mix` 8.701 ms（43.5%），`i12` 4.738 ms（23.7%）。
- **分塊 1024 px 把 PSRAM 成本拉回 5.005 ms**，只比全 internal 慢 5.6%，
  比整塊 PSRAM 快 **2.22×**。分塊 256 是 5.836 ms，分塊 64 反而 9.556 ms
  （function call／每塊固定成本太高）。**結論由 §7.147 的「256 px 一塊」修正為
  「1024 px 一塊」**（8 KB scratch，仍然在 L1）。

### 4.3 各階段 PSRAM 代價（9,600 px）

| 階段 | internal | PSRAM | 倍率 |
|---|---:|---:|---:|
| 8-bit 配對 | 2.773 ms | 3.127 ms | 1.13× |
| 12-bit 配對 | 2.612 ms | 5.306 ms | **2.03×** |
| 12→8 捨入 | 2.093 ms | 3.199 ms | 1.53× |
| 12→8 抖動 | 3.541 ms | 4.617 ms | 1.30× |
| 整條 12-bit | 4.738 ms | 11.095 ms | **2.34×** |

- 12-bit 配對最傷：每顆寫 8 B，PSRAM 的隨機寫入成本直接放大 2×。
- 抖動只放大 1.30×：它順序讀 12-bit、順序寫 8-bit，PSRAM 的 burst 較友善。
- 先做「12-bit 配對」的分塊，比先做「抖動」的分塊有效。

---

## 5. 對 8×8 的實際結論

1. **8×8 完全唔需要 PSRAM**：64 px 全 12-bit＋抖動 63.8 µs/幀，佔 50 FPS 預算 0.3%。
   畫布 512 B，internal 一定夠；落 PSRAM 只會多一次 cache line 的風險，冇好處。
2. **效果計算才是 8×8 的主成本**：33.7 µs／64 個值；如果每顆 LED 都要獨立值
   （`pixel_hint=64` 已經是），加大 `pixel_hint` 會線性上升。
3. **12-bit 畫布的分塊門檻在 4,096 px 左右**：≤960 px 唔使分；4,096 開始有感；
   9,600 px 必分，**1024 px 一塊**最平衡（5.0 ms vs 11.1 ms）。
4. **混合放置（12p→8i）是默認建議**：12-bit 的中間畫布可以落 PSRAM，
   但 8-bit 輸出（要交 encode／DMA）留在 internal；9,600 px 由 11.1 → 8.7 ms。
5. **多 job 成本線性**：2／4 job 1.37／2.73 ms（9,600 px、12-bit internal），
   2→4 個 job 多 1.37 ms（≈0.68 ms／job，主要是多一次配對），唔係免費但可接受。

---

## 6. 開發時踩到的兩個真 bug（已修）

1. **分塊 scratch 用絕對座標**：`nc_px_pair_apply12()` 的 `base` 是畫布絕對偏移；
   分塊時如果把 `base` 設成 chunk 的 `off`，`dst` 卻是 chunk 大小的 scratch，
   第二塊開始就會寫出界（960 px 剛好還在 8 KB 內，9,600 px 就爆）。
   修正：scratch 的 `base` 永遠 0，寫完再把 `c8_i + off*4` 交回去。
2. **`vTaskDelay(1)` 餵不到 idle**：decode task 優先級高過 idle，每次都在 tick
   邊界醒來，idle 永遠被搶先 → 量測跑幾秒就 `task_wdt`（`rst=6`）。
   改成 `vTaskDelay(2)`，留一個完整 tick 給 idle；量測數字不變。

---

## 7. 限制與下一步

- 只量到「效果 → 8-bit canvas」；**未量 encode／I80 送燈**（EffectPerf 不開 render）。
- 未量 network／ncm 同時跑時的 cache 污染；要量就把 EffectPerf 的 tasks.json
  打開 net／ncm 再跑同一組（但這就不再是「純效果」）。
- 抖動是 1D 誤差擴散（`nc_px_canvas12_to_8` 的 `err[4]`），未量 temporal／spatial 差異。
- 下一步：把 1024 px 分塊接去真正的 producer（`nc_px_show_frame` 之後、
  encode 之前），量「分塊＋encode＋I80」的端到端；以及 apa102 16-bit／
  pca9685 12-bit 直出（唔使 12→8）。

---

## 8. 記憶體牆與 CPU 預算（9,600 px 的實際算式）

### 8.1 「全部緩衝」到底幾大

| 緩衝 | 64 px | 960 px | 4,096 px | 9,600 px |
|---|---:|---:|---:|---:|
| 8-bit 畫布（4 B/px） | 256 B | 3,840 B | 16,384 B | **38,400 B** |
| 12-bit 畫布（8 B/px） | 512 B | 7,680 B | 32,768 B | **76,800 B** |
| 兩塊同時 | 768 B | 11,520 B | 49,152 B | **115,200 B（112.5 KiB）** |
| 分塊 1,024 px 的 12-bit scratch | — | — | — | **8,192 B** |
| 分塊 1,024 px 的 8-bit 輸出 | — | — | — | **4,096 B** |

- **EffectPerf 隔離映像**：開機 `h_int = 234,687 B`，9,600 px 的
  `c12i=1`、`c8i=1` — **115,200 B 全部放 internal 都夠**（實測）。
- **滿載映像**：`h_int` 約 102 KB（tests 映像 102,119 B），115,200 B 已經放不下；
  再加上 render 的 3 個 DMA 槽（16×600 一槽 86,800 B → **260.4 KB**）與環，
  **量產 16×600 一定要 PSRAM**。
- 但「必須 PSRAM」不等於「全部都要 PSRAM」：如果 producer 分塊（1,024 px），
  工作集只有 **8 KB 12-bit scratch ＋ 4 KB 8-bit 輸出 = 12 KB internal**；
  大畫布可以留在 PSRAM，或者根本不存整塊（每塊即算即送）。

### 8.2 50 FPS（20 ms）之下，Effect 用完仲剩幾多

| workload（9,600 px 或指定 px） | Effect CPU | 剩餘 / 20 ms | 剩餘 / 16.67 ms（60 FPS） |
|---|---:|---:|---:|
| 64 px（8×8） | 0.064 ms | **19.94 ms（99.7%）** | 16.60 ms（99.6%） |
| 960 px | 0.501 ms | 19.50 ms（97.5%） | 16.17 ms（97.0%） |
| 4,096 px internal | 2.039 ms | 17.96 ms（89.8%） | 14.63 ms（87.8%） |
| 9,600 px internal | 4.738 ms | **15.26 ms（76.3%）** | 11.93 ms（71.6%） |
| 9,600 px 分塊 1,024 | 5.005 ms | **15.00 ms（75.0%）** | 11.66 ms（70.0%） |
| 9,600 px 混合（12p→8i） | 8.701 ms | 11.30 ms（56.5%） | 7.97 ms（47.8%） |
| 9,600 px 全 PSRAM | 11.093 ms | 8.91 ms（44.5%） | 5.57 ms（33.4%） |

> 這是 **Effect 管線本身**的 CPU；未計 encode／I80。EffectPerf 的 render 是關的。

### 8.3 如果要連 encode／送燈

- **8×8**：encode `enc_us ≈ 348 µs`（Matrix 實測）＋ effect 64 µs ≈ **0.41 ms/幀**，
  50 FPS 之下仍然剩 **~19.6 ms**，非常鬆。
- **9,600 px（16×600）**：encode 實測 **18.4–20.1 ms**（PSRAM、舊架構；
  WiFi off 的 15.4 ms 版本仍然 >15 ms）。所以：
  - 同一個 core 做 effect＋encode：4.7–11.1＋18.4–20.1 = **23–31 ms**，
    50 FPS **一定超支**；
  - 分核（producer core0、render core1）：effect core 仍有 **8.9–15.3 ms**，
    但 render core 只剩 **0–1.6 ms**；要去到 33–40 FPS（25–30 ms 週期）
    render core 才有 5–12 ms 剩。
- 所以「CPU 還剩多少」要分開講：
  - **8×8**：effect 之後幾乎全部時間（19.6/20 ms）都可以做其他事；
  - **9,600 px**：effect 之後 producer core 有 8.9–15.3 ms，但 encode 那邊
    本身已接近 20 ms，係另一個牆。

### 8.4 結論

1. **9,600 px 的「整塊 12-bit＋整塊 8-bit＋DMA 槽」在量產映像一定要 PSRAM**；
   EffectPerf 隔離後 internal 放得落，只證明「不是畫布絕對太大」，不是量產配置。
2. **分塊 1,024 px** 把 internal 工作集由 115 KB 壓到 **~12 KB**，同時把 PSRAM
   全鏈由 11.09 → 5.00 ms；這是 9,600 px 的正路。
3. **Effect 本身在 9,600 px 只佔 23.7–55.5% 預算**（internal／PSRAM），
   producer core 仍有 8.9–15.3 ms；但 **16×600 的 encode 是另一個 18–20 ms 的牆**，
   要 50 FPS 必須分核＋分塊 submit，唔可以同 effect 擠同一個 core。

---

## 9. PSRAM 純搬運頻寬（0xF067 PSRAMBENCH；真機）

> EffectPerf 另外加了一條 `0xF067 PSRAMBENCH`（回 `0xF068`），只量 `memset`／
> sequential read／`memcpy`，不量效果。工具：`tools/psram_bench.py`。
>
> **這一支是 raw microbenchmark，直接用 `heap_caps_aligned_alloc`，不是正式 buffer 路徑。**
> 正式路徑一律用 `nc_buf`（[buffers.md](../03_notes/buffers.md)）：`where=S/P/DMA`、
> `slot_bytes × n` 開機定死、`reserve` 放語意 header、`put_end(len)` 記用量、
> `get(&len,&off)` 交邊界、`handover` 換 owner、`carve` 換場。

**量測條件**：S3 240 MHz、2 MB quad PSRAM @ 80 MHz、EffectPerf 映像（只有 nc4）、
buffer 64 B 對齊、每組 8 次（1 MB 用 2 次）；PSRAM cold read 每次先
`esp_cache_msync(M2C)`，write cold 每次 `C2M`。

| 大小 | internal wr / rd / cp | PSRAM wr / wr_cold | PSRAM rd / rd_cold | PSRAM cp | 混合 i2p / p2i / p2p |
|---:|---:|---:|---:|---:|---:|
| 64 KB | 725 / 129 / 361 MiB/s | 14 / 13 | 24 / 26 | 9 | 14 / 24 / 9 |
| 256 KB | —（internal 不足） | 13 / 13 | 25 / 26 | 9 | 13 / 25 / 9 |
| 512 KB | — | 13 / 13 | 25 / 26 | 9 | 13 / 25 / 9 |
| **1 MB** | — | 13 / 13 | 25 / 26 | —（只配一塊） | 13 / 25 / — |

- **PSRAM 順序讀 ≈ 25–26 MiB/s（26–27 MB/s）**，寫 ≈ **13 MiB/s**，
  copy ≈ **9 MiB/s**；1 MB 仍然係同一條水平線，冇明顯 cache 悬厓。
- **internal copy 361 MiB/s、read 129 MiB/s、memset 725 MiB/s**；
  PSRAM copy 比 internal 慢 **~40×**，read 慢 ~5×。
- **chunk 1/4/16/64 KB 全部 ~25 MiB/s**（純讀）→ PSRAM 係 **bandwidth-bound**，
  唔係 latency-bound；分塊細唔會慢，1–4 KB 反而 cache footprint 更細。
- **cold vs hot 幾乎一樣**（26 vs 25）——64 KB 已超過 L1，重讀都係由 PSRAM 來；
  `esp_cache_msync` 在 256 KB–1 MB 約 **C2M 17 µs、M2C 12 µs**，相對傳輸可忽略
  （64 KB 第一次 178 µs 是 warm-up，第二次已回落）。
- 換算：9,600 px 12-bit 畫布 76.8 KB → 讀 **≈3.0 ms**、寫 **≈5.6 ms**、
  copy **≈8.3 ms**；同 §4 的 effect `p12 11.1 ms`、`mix 8.7 ms`、分塊 5.0 ms 對得上。
  1 MB → 讀 40 ms、寫 77 ms、copy 111 ms：**大 buffer 只可以放 PSRAM 做冷資料，
  唔可以每幀全掃**。

**由此定案**：

1. 大／冷 buffer（full 12-bit canvas、DMA slots、asset arena）放 PSRAM；
2. 每幀熱工作集（1,024 px chunk、8 KB 12-bit scratch、4 KB 8-bit 輸出）留 internal；
3. 由 PSRAM 讀入 internal 用 **1–4 KB chunk** 就夠（頻寬一樣，latency 更低）；
4. 寫 PSRAM 比讀貴一倍（13 vs 25 MiB/s），所以 **pair/dither 在 internal 做，
   只把最終結果寫 PSRAM**；唔好每幀喺 PSRAM 上做多次 read-modify-write；
5. `esp_cache_msync` 的固定成本（~12–17 µs）遠低於搬 256 KB（~10 ms），
   但 **DMA 前一定要 sync**，而且 sync range 要 round 到 64 B。

---

## 10. 9,600 個效果值 → 配對 → 12→8：整條生產線（0xF069）

> 之前的 0xF065 只算 **64 個值**（8×8 的真實用法），所以「效果生成」看起來只 33.7 µs。
> 16×600 要**每顆 LED 一個波**時，生成器要跑 9,600 次（RGB 28,800 次）；
> 這一支 `0xF069 PIPEBENCH` 把 `gen`／`pair`／`conv` 分開量。
> **RGBW→I80 編碼／送燈不在這裡**（render backend 已有），避免混入已知成本。

payload `[px:u16][frames:u16][mode:u8]`；mode 0=1 值/px（wwww）、1=3 值/px（rgb）、
2=1 值/px（w）；回 `0xF06A`。工具：`tools/pipe_bench.py`。

### 10.1 真機（9,600 px、S3 240 MHz、12-bit canvas internal、frames=2）

| 階段 | 1 值/px（wwww） | 3 值/px（rgb） | 備註 |
|---|---:|---:|---|
| `gen`（`nc_px_job_vals`） | **5.155 ms** | **15.587 ms** | 約 **537–541 ns/值** |
| `pair` range（`nc_px_pair_apply12`） | 4.790 ms | 4.894 ms | 約 **499 ns/px** |
| `pair` 連續 table | 5.302 ms | 5.303 ms | 比 range 慢 ~11% |
| `pair` 打亂 table | **19.941 ms** | **19.939 ms** | 4.2× range；非連續寫入好貴 |
| `conv` 12→8（捨入） | 3.425 ms | 3.426 ms | 同值數無關（按 pixel×4 通道） |
| **full（gen＋pair range＋conv）** | **13.530 ms** | **24.167 ms** | 20 ms 的 **67.7%**／**120.8%** |
| 50 FPS 剩餘 | **6.47 ms** | **−4.17 ms** | RGB 28,800 值超支 |

- 1 值/px：**gen 38%、pair 35%、conv 25%**——三段都重要，冇單一壓倒性瓶頸。
- 3 值/px：**gen 65%**、pair 20%、conv 14%——生成器是主瓶頸。
- `pair` 的 W 與 WWWW **同價**（4.77 vs 4.79 ms）：成本在 `k % nv`／range 迴圈，
  不是「寫 4 個通道」的記憶體頻寬。
- 連續 table 比 range 慢 11%；**打亂 table 4.2×**。所以「表」本身唔貴，
  貴的是**非連續目的地**；配對器應該用 **runs（連續段）**，唔好逐顆 form table。

### 10.2 判讀

1. **9,600 個 1 值波（wwww）在 50 FPS 可以**：13.53 ms，留 6.47 ms 給 I80／其他；
   60 FPS 只剩 3.14 ms，較緊。
2. **9,600 px 真 RGB（28,800 值）在 50 FPS 不行**：24.17 ms，超 4.17 ms；
   生成器 15.59 ms 是主因；pair＋conv 只 8.32 ms。
3. **最直接的優化是波表快取**：MP 的 `effect_core.py` 有 `_WAVE_CACHE`，
   C 這邊 `program_value` 是逐值重算（537 ns/值）。把 `program` 的 `total`（256）預算成
   `uint16_t wave[256]`，每幀只做 `wave[(ph + i*spacing) % total]`，
   生成可望由 5.16/15.59 ms 降到 **<1 ms**；之後 pair（4.8）＋conv（3.4）才是主成本。
4. **pair 的 `% nv` 可以省**：當 `nv >= job->nled`（每顆有自己的值）時，
   `vals[k]` 直接索引，不需要 `k % nv`；對 9,600 px 是每幀 9,600 次除法。
5. **table 只用於非連續 group**；連續 job 用 range；非連續先把目的地排序成 runs，
   或分塊（每塊內部連續）再寫，避免 19.9 ms 的 random scatter。

> `full_ps`（canvas 在 PSRAM）今次沒有數字：`c12p` 在這次配置下配不到
> （回覆 `psram12:—`）；之前 0xF065 的 `p12=11.1 ms`、`mix=8.7 ms` 仍然有效。
