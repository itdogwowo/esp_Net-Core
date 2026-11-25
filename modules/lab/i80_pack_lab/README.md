# i80_pack_lab — Task 6 I80 波形合成 A/B（一次性 lab）

> **性質**：一次性量測，唔係任務、唔會收納入固件。量完可以整包移除。
> 對應 [Task 6](../../../docs/00_pm/pixel-tiers-plan.md)、
> [status §7.220](../../../docs/00_pm/status.md)。

## 目的

比較 I80 encode 兩條路徑：

- **舊**：每條 lane `nc_px_codec_wave_pack()`（packed bitstream）＋`merge8` 轉置；
- **新**：`nc_px_codec_pack_bus()` 每顆即時取 lane 位元組 → 8×8 SWAR 轉置 → 逐 tick 寫 bus word。

## 結果（2026-11-24，ESP32-S3 4 MB／2 MB PSRAM，EffectPerf 映像）

| px | 舊 | 新 | 倍數 | bus_ok |
|---|---:|---:|---:|---:|
| 9,600（16×600） | 24,018 µs | **6,365 µs** | **3.77×** | 1 |
| 2,400（16×150） | 4,765 µs | **1,166 µs** | **4.09×** | 1 |

reps=16 重跑：6,365／1,166 µs，同 reps=8 一樣。

> **未做**：16-lane × 16-bit 實機（冇板）。呢個 lab 的 16-lane 數字係同一份 C
> 在板上跑，但未經真實 16 條線輸出。

## 怎麼跑

```bash
# 1) EffectPerf 映像（含 0xF077 與 lab variant 17／18）
IDF_PATH=$HOME/esp/esp-idf python3 make.py build ESP32-S3-Matrix \
    --project ESP32-S3-Matrix-EffectPerf

# 2) OTA（唔使 BOOT）
python3 tools/ota.py --port /dev/cu.usbmodemXXXX \
    artifacts/ESP32-S3-Matrix__ESP32-S3-Matrix-EffectPerf.bin

# 3) A/B
python3 modules/lab/i80_pack_lab/run_ab.py --port /dev/cu.usbmodemXXXX \
    --px 9600 --variant 17 --reps 8

# 12-bit dither（完整轉置）
python3 modules/lab/i80_pack_lab/run_ab.py --port /dev/cu.usbmodemXXXX \
    --px 9600 --variant 18 --reps 8
```

## 代碼在哪

- **firmware 側**：`firmware/components/bench/effect_bench/nc_layout_bench.c`
  的 `variant == 17 || variant == 18` 段。它只存在於 test-only 的 `effect_bench`
  component（EffectPerf 映像），**唔入 production**。
- **host 側**：本目錄 `run_ab.py`。主體 `tools/layout_bench.py` 保持通用，
  冇 lab variant。

## 清理

16-lane 驗證完之後：

1. 把結果寫入 `docs/00_pm/status.md`／`test-register.md`；
2. 可以移除 firmware 的 variant 17／18 段（或把 diff 留在本目錄做 patch）；
3. 本目錄可以整包刪除。
