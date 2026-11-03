# 記憶體預算（ESP32-S3）

> **用途**：回答「這個系統一定要用幾多 SRAM、開機之後還剩幾多、同 MicroPython 比係多定少、
> 開 WiFi 會再食幾多」。
> **數字來源**：`firmware/build/esp_net_core.map`（**2026-11-03 重新 build**，ESP-IDF v6.1.0，
> target esp32s3）+ Espressif 官方 Wi-Fi 文件 + 對 Wi-Fi 靜態庫的 `size -A` 實測。
> **不是**在板子上即時量到的 —— 板子目前沒有可用的 console（見 §8）。
> 相關：[decode-routes.md](decode-routes.md) §3（各緩衝在哪）、[task-lifecycle.md](task-lifecycle.md) §3

---

## 1. 先分清三個「RAM」

| 講法 | 是什麼 | S3（N16R8） | 特性 |
|---|---|---|---|
| **內部 SRAM**（= 我講的「內部 RAM」） | die 裡面那塊 | **512 KB**（本 build 可用 334 KB DRAM + 350 KB IRAM） | 快、可 DMA、ISR 一定要用 |
| **PSRAM** | 板外那顆，經 SPI/cache | **8 MB** | 慢、**不可 DMA**、當 heap 用 |
| Flash | 程式與常數 | 16 MB | 唯讀（除非 OTA） |

> 注意：512 KB 是**總量**，不是全部給應用。IDF 會先拿走 cache、ROM 保留、bootloader 等，
> 本 build 實際可用的 DRAM 段只有 **334 KB**。

---

## 2. S3 實測（map 檔）

| 段 | 大小 | 說明 |
|---|---|---|
| `dram0_0_seg`（可用 DRAM） | **341,760 B**（333.8 KB，`0x53700`） | 靜態 + heap |
| ├ `.dram0.dummy` | 42,496 B | IDF 保留（不是我們的） |
| ├ `.dram0.data` | 23,543 B | 有初始值的全域 |
| └ `.dram0.bss` | 42,424 B | 零初始化全域（我們的緩衝多數在這） |
| **靜態合計** | **108,463 B（105.9 KB）** | 佔 DRAM 段 31.7% |
| **開機可用 heap** | **233,296 B（227.8 KB）** | 68.3% |
| `iram0_0_seg` | 358,144 B（349.8 KB，`0x57700`） | 指令用，不是 heap |
| └ 已用（vectors 1,028 + text 57,703） | 58,731 B（57.4 KB） | 16.4% |
| Flash `.text` + `.rodata` | 324,440 B（316.8 KB） | 程式碼與常數 |
| 燒錄 bin | 408,256 B（398.7 KB） | 含內嵌預置 JSON 與對齊填充 |

**一句話：S3 build 開機時靜態用 105.9 KB，剩 227.8 KB heap。**

> **2026-11-03 一天之內省了 19,120 B（18.7 KB）**，全部來自清掉 handler 的 static 緩衝：
> `reply_json` 的 2,102 B ＋ status/bus 那 8,864 B ＋ file_action 的 `s_tx[8,205]` 與 `buf[2,048]`。
> 做法一律是「砌入回覆幀（棧）或用 heap」，所以**兩個 decoder 同時回覆不會再撞**。
> 詳見 [status.md](../00_pm/status.md) §7.1 的 A 與 Q–S。

---

## 3. 靜態佔用的主要項目（`nm -S --size-sort` 實測）

| 符號 | 大小 | 屬於 | 可以減嗎 |
|---|---|---|---|
| `_cdcd_itf` | 12,532 | **tinyusb** CDC 介面結構 | ⚠️ 要 USB 就要 |
| `payload$3` | 8,192 | tx_bench 測試填充塊 | ✅ 只有跑 `0xF007` 才用 |
| `s_slots` | 7,680 | task manager 20 個 slot | ⚠️ 20 × 384 B，可以減 slot 數 |
| `s_entries` | 2,304 | 檔案表（`/data` 快取） | ⚠️ |
| `s_mods` | 2,016 | FXE 模組實例表 | ⚠️ |
| `s_buses` | 1,984 | nc4 8 條 bus | ⚠️ |
| `s_provs` | 960 | provides 註冊表 | ⚠️ |
| `ledger$10` / `pj$11` | 900 / 700 | **只在 `app_main` 印一次 log** | ✅ 開機用完即棄 |
| `pxReadyTasksLists` / `_mscd_epbuf` / `port_IntStack` … | IDF / FreeRTOS | 不是我們的 |

> **最大的一塊不是我們的**：tinyusb（CDC + MSC）合計 21.2 KB（`_cdcd_itf` 12,532 ＋
> `_mscd_epbuf` 8,192 ＋ `_cdcd_epbuf` 1,024），比整個 nc4 元件（2,187 B）多十倍。
> 如果哪天要省 SRAM，先問「USB 一定要開嗎」比問「decoder 棧可不可以細」有效得多。
>
> ✅ **handler 的 static 已經清完（2026-11-03）**：`json` / `part` / `plan` / `hubs` / `prov` /
> `buses` / `ws` / `udp` / `tasks` / `st` / `s_tx` / `buf` 全部消失（合計 −19,120 B）。
> 現在回覆一律「砌入呼叫者棧上的回覆幀」或「送出時才 malloc」，所以**兩個 decoder 同時回覆不會互踩**。
> 剩下 `payload`（bench）與 `pj`/`ledger`（開機 log）與併發無關。
>
> ⚠️ **WS / UDP 的 41 KB 不在這張表**：它們在 `components/sys/transport/`，只在 P4 被引用，
> S3 build 的連結器不會把它們拉進來。P4 build 要另外加 ~41 KB BSS。

---

## 4. 執行期佔用（開機後從 heap 拿）

| 項目 | 大小 | 來源 |
|---|---|---|
| `decode0` + `decode1` 棧 | 2 × 6,144 = 12,288 | `s_nc4_loops` |
| `slow_worker` 棧 | 4,096 | `s_core_loops` |
| `fx` 棧 | 4,096 | `s_fx_loops` |
| `hello` 棧 | 3,072 | `nc_hello` |
| `nc4.rx`（parser） | 8,205 | plan（INTERNAL） |
| `uart.rx` | 1,024 | plan（INTERNAL） |
| `jobs`（8 格慢工作） | ~192 | plan（INTERNAL） |
| `probe.hub` | 512 | plan（INTERNAL） |
| main task 棧 + idle/timer | ~6,500 | FreeRTOS |
| **合計** | **~40 KB** | |

`fx.arena` 512 KB 在 **PSRAM**，不佔內部 SRAM。

**開機穩定後剩餘：約 227.8 − 40 ≈ 188 KB（±10 KB）。**

---

## 5. 「不可避免」的是幾多

| 分類 | 大小 | 可不可以減 |
|---|---|---|
| IDF 保留（`.dram0.dummy`） | 42.5 KB | ❌ 不可 |
| 我們的全域緩衝（含 tinyusb 21.2 KB） | 42.4 KB | ✅ 部分可以搬去 PSRAM 或改成 plan |
| 任務棧 | 23.5 KB | ✅ 可以減（但要先量 high-water） |
| plan 區（parser / jobs） | ~10 KB | ⚠️ 要收資料就一定要 |
| **合計** | **~118.4 KB** | |

所以「這個系統跑起來」的地板大約 **130–140 KB 內部 SRAM**，其餘 ~170 KB 是餘裕。

---

## 6. 同 MicroPython 比

### 6.1 靜態佔用（同一塊 S3，`idf.py size` 的數字）

| | 我們的固件（本 build） | MicroPython（S3，issue #10049 的 build） |
|---|---|---|
| 靜態 IRAM | **58,731 B**（57.4 KB） | **139,730 B**（136.5 KB） |
| 靜態 DRAM（`.data` + `.bss`，含 IDF 保留） | **108,463 B**（105.9 KB） | **67,137 B**（65.6 KB） |
| 靜態合計 | **167,194 B（163 KB）** | **206,867 B（202 KB）** |
| Flash 映像 | **408,256 B（398.7 KB）** | **1,964,037 B（1.87 MB）** |
| 執行期 | 任務棧 + plan ≈ 40 KB | GC heap 一整個（有 PSRAM 就整個搬去 PSRAM） |

**結論（靜態）**：MicroPython 的靜態佔用比我們多 **~40 KB**，而 flash 是我們的 **4.8 倍**
（1.87 MB vs 398 KB）—— 因為它要帶解譯器 + 一整套 frozen 模組。

> 注意：兩邊 IDF 版本、分區表、有沒有開 PSRAM 都不同，所以「remain」那一欄不可直接比；
> 這裡只比「used」。數字來自使用者的 build 輸出，**不是本地量測**。
>
> 另外 §5 的「地板 ~137.5 KB」是**我們自己用掉的**（不含 IDF 保留的 42.5 KB 與任務棧的算法差異），
> 跟這裡的 163 KB 是兩個不同的切法，不要相加。

### 6.2 為什麼「剩幾多」仍然不能直接比

| | 我們的固件 | MicroPython（S3） |
|---|---|---|
| 執行模型 | 原生 C，編譯期決定 | 位元組碼 + 解譯器 |
| 內部 SRAM 主要用途 | 固定緩衝 + 任務棧（每塊有名字） | 解譯器 + GC heap + WiFi/LWIP |
| heap 形態 | 用完即還，可預測 | 一個大 GC pool，碎片化 |
| PSRAM | 只放 FXE arena（512 KB） | 有 PSRAM 時 **GC heap 整個搬去 PSRAM** |
| 8 MB 板上的 `gc.mem_free()` | — | 約 **7.9 MB**（在 PSRAM，不代表內部 SRAM 餘裕） |

MicroPython 一開機就會向 ESP-IDF 要一大塊 heap 做 GC pool（PSRAM 有的話就從 PSRAM 要），
所以「`gc.mem_free()` 還有 7.9 MB」講的是 PSRAM，不是我們在意的內部 SRAM。

**總結**：靜態我們贏（少 ~20 KB、flash 少 4.8 倍），內部 SRAM 的**可預測性**我們也贏
（每一塊都有名字、可以逐項列出來）；MicroPython 贏的是「寫應用的門檻」與「heap 可以無腦大」。

> 引用：[micropython#10049](https://github.com/micropython/micropython/issues/10049)（S3 的 build 統計）。

---

## 7. 開 WiFi 會多幾多

### 7.1 先講一個容易搞錯的事實：**現在的 image 完全沒有 Wi-Fi**

`sdkconfig` 有 `CONFIG_ESP_WIFI_ENABLED=y`，但**沒有任何元件 `REQUIRES esp_wifi`**，
所以 `libnet80211.a` / `libpp.a` / `libesp_wifi.a` 只是被放在連結命令列上（map 裡只有 `LOAD` 行），
**一個成員都沒有被拉進來**（實測：map 中來自這幾個 archive 的 section 貢獻 = **0**）。

即係 §2 的 124.6 KB 靜態、209.2 KB heap 是**「Wi-Fi 未開」**的數字。
要開 Wi-Fi 是「加」不是「已經包在裡面」。

### 7.2 靜態部分（實測，`size -A` 對 Wi-Fi 靜態庫）

| archive | `.bss` | `.data` |
|---|---|---|
| `libnet80211.a` | 8,111 | 1,042 |
| `libpp.a` | 1,843 | 2,179 |
| `libsmartconfig.a` | 641 | 56 |
| `libesp_wifi.a` | 81 | 480 |
| `libespnow.a` / `libcore.a` / `libwapi.a` / `libesp_coex.a` | 182 | 99 |
| **合計（上限）** | **10,777** | **3,856** |

**靜態上限 14.3 KB**（全部成員加總；實際只會更少，因為連結器只拉用到的成員）。
所以「Wi-Fi 的程式碼與全域變數」本身很便宜 —— 貴的從來不是這個。

### 7.3 執行期部分（官方緩衝等級表）

Espressif 官方 `wifi.rst` 對 **ESP32-S3** 給的緩衝等級表（可用記憶體 = Wi-Fi 初始化後剩幾多）：

| 等級 | 可用記憶體 | 相對最小等級 |
|---|---|---|
| Iperf（極限效能） | 133.9 KB | −139.7 KB |
| **預設** | **183.9 KB** | −89.7 KB |
| 最小 | 273.6 KB | 0 |

即係：**Wi-Fi 協定棧的執行期緩衝（驅動 RX/TX + LWIP 視窗）在預設等級大約食 90–120 KB**，
調到「最小」等級可以壓到 ~50–60 KB，代價是吞吐量大降（TCP 64 → 23 Mbit/s）。

### 7.4 合起來的帳

| | 靜態 | 執行期 | 合計 |
|---|---|---|---|
| 預設等級 | ~14 KB | ~90–120 KB | **~105–135 KB** |
| 最小等級 | ~14 KB | ~50–60 KB | **~65–75 KB** |

對我們的意義：

- 現在剩 ~188 KB → 開 Wi-Fi 預設等級之後大約剩 **53–83 KB**，仍然跑得動，但沒有餘裕再做別的事。
- 開到「最小」等級則剩 **~113–123 KB**，比較像樣。
- 如果同時要 ETH + Wi-Fi，就要把 `TCP_SND_BUF` / `TCP_WND` 調小，並把 WS/UDP 的 41 KB BSS 搬去 PSRAM。
- **要開 Wi-Fi 之前先做兩件事**：把 tinyusb 的 21.2 KB 與 `payload`（bench 的 8 KB）
  搬去 PSRAM 或改成「要用才 allocate」，否則 53 KB 餘裕會被一次大 JSON ＋ 一次 OTA 吃掉。

> 引用：Espressif ESP-IDF `api-guides/wifi.rst`（ESP32-S3 緩衝等級表）；
> 靜態數字為本地對 `components/esp_wifi/lib/esp32s3/*.a` 跑 `size -A` 的實測。

---

## 8. 之後怎麼量到真實數字

板子目前沒有可用 console（COM11 讀不到東西），所以以上是 map 檔 + 官方文件 + 靜態庫實測。要拿到實測：

1. 燒一個有 console 的 build（UART0 GPIO43/44，115200）。
2. 開機 log 應該有 `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)`；
   沒有的話在 `app_main` 尾加一行：
   ```c
   ESP_LOGI(TAG, "heap internal=%u psram=%u",
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
   ```
3. 開 WiFi 前後各印一次，差值就是 §7.3 的實測版。
4. 把數字填回 §2 / §4 的表，並把「估算」字樣刪掉。

### 8.1 這份文件的數字是怎麼產生的（可重現）

```bash
# 在 WSL 內，用 ESP-IDF v6.1.0
. <idf>/export.sh
cd <repo>/firmware
idf.py build
# 段大小
sed -n '/^Memory Configuration/,/^Linker script/p' build/esp_net_core.map
# 符號大小
nm -S --size-sort build/esp_net_core.elf | sort -k2 -r | head -30
# Wi-Fi 靜態庫
xtensa-esp32s3-elf-size -A <idf>/components/esp_wifi/lib/esp32s3/libnet80211.a
```

> 注意：`firmware/build/` 是**跨平台共用**的。Windows 上沒有 IDF，所以 build 一定要在 WSL 做；
> 在 Windows 直接開 `build/` 只會看到上次 WSL build 的殘留。
