# 記憶體實測：本專案 vs MicroPython（mp_Net-Core）

> **用途**：回答「現在靜態用幾多、開機剩幾多、同 MicroPython 版差幾遠、佢邊樣嘢值得抄」。
> **數據來源**：`firmware/build/esp_net_core.elf` 的 `.map` 段表 + `size -A`（**2026-11-06 23:53 build**，
> ESP-IDF v6.0.3，target **esp32p4**）；MicroPython 側取 `mp_Make-Tools/lib/micropython/ports/esp32/build-ESP32_GENERIC_*/`
> 的 `micropython.elf`（2026-08 build，同一個 `size` 工具量）。
> **不是**板上即時量到的；板上數字見 [memory-budget.md](memory-budget.md) §4 與開機 log「開機剩餘」。
> 相關：[buffer-manager-status.md](buffer-manager-status.md)（機制現況）、[memory-budget.md](memory-budget.md)（舊的 S3 帳）

---

## 1. 現在（P4，2026-11-06 23:53）

| 段 | 用 | 長 | 佔 |
|---|---|---|---|
| `sram_low`（0x4FF00000） | **128,976** | 179,152 | 72.0% |
| ├ `.iram0.text` | 68,224 | | |
| ├ `.dram0.bss` | 38,904 | | |
| └ `.dram0.data` | 21,837 | | |
| `sram_high`（0x4FF40000） | 0 | 393,216 | 0%（全部入 heap） |
| `irom_seg`（flash） | 460,252 | | |
| `drom_seg`（rodata） | 126,012 | | |
| **燒錄 bin** | **677,168** | | |

- **開機可用 heap ≈ 443,392 B（433 KB）** = `sram_low` 剩 50,176 + `sram_high` 393,216。
- **plan 之後 ≈ 397,702 B（388 KB）**：`usb.rx` 4,192 + `uart.rx` 1,024 + `tcp.rx` 16,410 +
  `probe.hub` 512 + 任務堆疊 ~23,552（decode 2×6,144 + slow 4,096 + fx 4,096 + hello 3,072）。
- **PSRAM 32 MB**（hex 200 MHz）靜態 0；`fx.arena` 512 KB 開機由 plan 從 PSRAM 拿，其餘幾乎全空。
- 靜態最大三塊仍然是 USB：`_cdcd_itf` 12,548 + `_mscd_epbuf` 8,192 + `_cdcd_epbuf` 1,024 ≈ 21.3 KB，
  再加 `s_slots` 6,000、`xIsrStack` 3,072、`s_entries` 2,304、`s_buses` 2,240。

> ⚠️ `firmware/build/` 一個目錄只放一個 target。上面這份 P4 build **覆蓋**了 23:51 的 S3 build；
> 要重跑 S3 的數要再 `python3 make.py build s3` 一次。

## 2. S3（2026-11-06 23:51 build，量完即被 P4 覆蓋）

| 項 | 值 | 對比 [memory-budget.md](memory-budget.md)（2026-11-03） |
|---|---|---|
| `.dram0.dummy`（IDF／ROM 保留） | 42,496 | 42,496（不變） |
| `.dram0.data` | 23,310 | 23,543 |
| `.dram0.bss` | 31,984 | 42,424 |
| **靜態合計** | **97,792（95.5 KB，28.6%）** | 108,463（105.9 KB）**−10,671** |
| **開機 heap** | **243,968（238 KB）** | 233,296（227.8 KB）**+10,672** |
| `iram0` | 58,807 / 358,144 | 58,731 |
| bin | 400,992 | 408,256 |

即 §3 那次「handler static 清乾淨」之後，BSS 由 42,424 落到 31,984；memory-budget.md 的表格已經過時
（數字仍是舊 build 的），但結論方向不變。

## 3. MicroPython 側（同一個 `size` 工具，2026-08 build）

| build | 靜態（DRAM/SRAM） | 佔比 | 開機可分配 | iram | bin |
|---|---|---|---|---|---|
| MP-S3（無 PSRAM） | 143,288 | 41.9% | 198,472 | 110,336 | 1,800,880 |
| MP-S3-SPIRAM-OCT | 162,760 | 47.6% | 179,000 | 126,135 | 2,720,128 |
| MP-P4-C6_WIFI | 157,464（sram_low 85.9%） | 35.4% | 287,928 | 117,372 | 2,960,736 |

兩個要注意的差異，唔係「邊個寫得好」：

1. **L2 cache 取捨**：MP 的 P4 build 用 `CONFIG_CACHE_L2_CACHE_256KB`，本專案用 128 KB。
   P4 的 L2 由 SRAM 切出來，所以 MP 的 `sram_high` 只剩 262,144 B（本專案 393,216）。
   → MP 用 128 KB 內部 RAM 換 PSRAM／flash 吞吐；本專案把嗰 128 KB 留給 heap。
2. **`.dram0.dummy` 差 51 KB**（MP-S3 93,952 vs 本專案 42,496）：那是 IDF／ROM 保留區，
   MP build 開的元件（BT 等）比較多。比較時睇「靜態」同「開機可分配」，唔好睇 dummy。

## 4. 放在同一張表

| | 本專案 P4 | MPY P4 | 本專案 S3 | MPY S3 |
|---|---|---|---|---|
| 靜態用內部 RAM | **129 KB** | 157 KB | **98 KB** | 143 KB |
| 開機可分配 | **433 KB** | 281 KB | **238 KB** | 194 KB |
| 跑完 init 後 | 388 KB + 31.5 MB PSRAM | ~224 KB + PSRAM（GC 可長大） | ~204 KB | ~134 KB |
| flash bin | **677 KB** | 2.96 MB | **401 KB** | 1.80 MB |
| 分配失敗 | 逐項 `ok:false`，其他照跑 | `MemoryError` | 同左 | 同左 |
| 誰決定大小 | `nc_plan_declare` 宣告 | 執行期 `malloc` | 同左 | 同左 |

**結論**：本專案在「用幾多 RAM」上係贏的（靜態少 28–45 KB、bin 細 4.4–4.5 倍、heap 多 44–152 KB）。
MicroPython 值得抄的不是數字，係**政策同可觀測性**——下面五點。

## 5. MPY 的五個做法（對應檔案）

| # | 做法 | 在哪 | 本專案現況 |
|---|---|---|---|
| 1 | **單一分配入口**：全專案只有 `buffer_hub.py` 碰 `heap_caps`；`alloc_dma` 失敗自動 fallback `bytearray`，回傳 `(buf, is_dma)`，`free_dma` 配對 | `slave/lib/sys/buffer_hub.py` | ✅ 等價：`nc_plan` + `NC_MEM_DMA` class，但**冇 fallback**（配唔到就 `ok:false`） |
| 2 | **固定容量 + 狀態機**：`AtomicStreamHub` N 格 SPSC，`IDLE→READY→READING`，熱路徑零 malloc；N 就係預算旋鈕 | 同上 | ✅ `nc_hub` 同一個狀態機（多一個 `WRITING`），P4 2.0 µs/次 |
| 3 | **分層 ownership**：L0 分配 → L1 ring → L2 傳輸 → L3 協議 → L4 應用 → L5 DMA，層與層只傳 `memoryview`，唔複製 | `doc/03_notes/02_buffer_architecture.md` | ⚠️ 有 plan/hub，但**冇寫成「邊層可以碰邊層」的規矩** |
| 4 | **大 buffer PSRAM-first、跨核 DMA-first**：framebuffer 走 `CAP_SPIRAM → CAP_DMA → bytearray`，ring 一律 `CAP_DMA` | `buffer_hub.py` §L0 + `05_psram_zero_block_plan.md` | ✅ `NC_PLAN_PSRAM` / `NC_PLAN_DMA`，但 policy 散落在呼叫端 |
| 5 | **隨時可查**：`gc.mem_free()`、`micropython.mem_info()`、`heap_caps.get_free_size / get_total_size / get_largest_free_block`，NC4 STATUS 直接回 `mem_free` | `mp_heap_caps` module | ⚠️ 只有開機一行 log + `0x1101 keys=mem` 的 plan/hub 報表；**STATUS 冇 PSRAM、冇最大連續塊** |

MicroPython 自己那層（`ports/esp32/main.c`）：`gc_init(heap, heap+64KB)`，配 `MICROPY_GC_SPLIT_HEAP_AUTO`
按需要再向 `malloc` 要新區塊（大塊自然落 PSRAM），所以 8 MB PSRAM 板 `gc.mem_free()` 報 7.24 MB。
**呢個「可長大」係彈性，同時係不可預測**——本專案的 plan 正正係為咗避開佢。

## 6. 可以抄的五件事（按成本排序）

1. **STATUS 補 `mem2`**（半日）：`{int:{free,largest}, psram:{free,largest}, plan:{declared,allocated,fail}}`。
   對齊 MPY 的 `get_free_size` / `get_largest_free_block`，順手把開機那行 log 變成可查。
2. **hub 報表加佔用率**（半日）：`nc_hub_fill()/nc_hub_depth()` 已經有，registry JSON 加 `"fill":k,"depth":n`，
   就係 MPY 講嘅「同時維持幾條鏈路」嗰個旋鈕。
3. **PSRAM-first 寫成 policy 表**（一個鐘）：邊類 buffer 一定 PSRAM、邊類一定內部 DMA、邊類可以 fallback，
   同 `buffer-manager.md` §6 對齊；避免每個呼叫端自己決定。
4. **「熱路徑零 malloc」變成檢查**（一日）：MPY 有 `Skills/buffer-conventions` 做規矩；本專案可以在
   `tools/ncbuild` 加一條 grep（decode/transport 路徑唔准出現 `malloc`/`calloc`）。
5. **每次 build 存一份 size 報表**（兩個鐘）：`artifacts/<board>.json` 已經有 sha256/尺寸，加 `static_bytes`、
   `heap_bytes`、`largest_free` 就可以睇趨勢——MPY 只存 bin，呢樣可以做得比佢好。

## 7. 怎麼重跑

```bash
# P4
python3 make.py build p4
~/.espressif/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin/riscv32-esp-elf-size -A firmware/build/esp_net_core.elf
sed -n '/^Memory Configuration/,/^Linker script/p' firmware/build/esp_net_core.map

# S3（會覆蓋 P4 的 build 目錄）
python3 make.py build s3
~/.espressif/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin/xtensa-esp32s3-elf-size -A firmware/build/esp_net_core.elf
```

板上數字（真正嘅「佔用」）要開 console 睇 `開機剩餘：內部 … / PSRAM …`，或者 `0x1101 keys=mem`。
