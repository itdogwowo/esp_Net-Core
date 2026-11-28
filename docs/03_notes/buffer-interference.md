# 緩衝位置與干擾（為將來的統計做準備）（2026-11-27）

> 圖：[`buffer-locations.svg`](../00_pm/buffer-locations.svg)（`tools/buffer_locations.py`）。
> 一句話：**緩衝住邊，決定咗你量到嘅數字係「佢自己」定「佢＋鄰居」。**

## 1. 位置原則（現況，實測 SuperMini、release）

| 記憶體 | 住什麼 | 為什麼 |
|---|---|---|
| **Internal SRAM** | `static .bss/.data` 143,559 B、`usb.rx` 4,128、環（`i80.prd` 576／`mock.prd` 576）、任務 stack | CPU 快、DMA 可用；Wi-Fi／lwIP 亦喺度 |
| **DMA-capable internal** | I80／SPI 後端槽（≤48 KB 才放） | **S3 唔可以 DMA 讀 PSRAM**；環唔係 DMA 讀的對象 |
| **PSRAM** | `nc4.reply0` 2,615（每 decoder）、`ncm.arena`（有模組 32 KB 起）、`s_recs` 768 | 大／冷；回覆 JSON 每條最多 2,600 B |
| **Flash（XIP）** | schema 字典 29,735 B（keys 表 10,296＋字串）、程式、資產 JSON | 0 RAM；但讀嘅時候爭 flash cache |

**Kconfig 政策**：環放邊由 `CONFIG_NETCORE_PIXEL_RING_PSRAM` 決定（build-time）。
環係 CPU 寫、CPU 讀；DMA 讀的是**後端自己的 internal 槽**——所以環放 PSRAM 都唔會
直接俾 DMA 讀，9,600 px 專案會多一段「PSRAM 環 → internal 槽」的 copy 成本。

## 2. 六條干擾路徑（統計量測會撞到）

| 路徑 | 誰 ↔ 誰 | 機制 | 症狀 |
|---|---|---|---|
| PSRAM 帶寬 | `reply0` ↔ PSRAM 環 | 兩邊都讀寫 PSRAM；cache line 爭用 | RTT ↑、FPS 抖、`bus_ok` 仍 1（唔會報錯） |
| Internal 爭用 | parser／stack ↔ Wi-Fi／driver | Wi-Fi 起來食 internal（數十 KB） | heap pool 縮水 → cache miss ↑、延遲抖 |
| DMA 池 | I80／SPI 槽 ↔ 其他 DMA | S3 只有 internal DMA；槽位有限 | 搶唔到就逐格降級（明示 skipped，唔會靜默） |
| Flash cache | schema／資產 JSON ↔ code fetch | XIP 讀同一條 flash | 只影響開機／首次載入，之後 cached |
| 鎖／所有權 | `nc_buf` 格狀態、FAT 鎖、bus 鎖 | 量測同時做 FILE／MSC | RTT 一定被拖（幾 ms 級） |
| 量測自身 | `0xF061`／`0xF081` | 多一次 PSRAM 寫＋USB 傳輸 | 細 RTT 量測被自己污染 |

## 3. 對統計的具體影響

- **RTT**：回覆 JSON 砌在 PSRAM（`reply0`）→ 如果同時有 PSRAM 環在跑，RTT 會比單獨量高；
  量測時要寫明「有冇同時開 pixel producer／Wi-Fi／MSC」。
- **FPS／frametime**：效果生產者寫 PSRAM 環，同 `reply0`／`s_recs` 争同一條 PSRAM；
  `0xF081` 的 `overrun`／`bad` 要連同「當下有咩在跑」一齊記錄。
- **編碼／推送（`bus_ok`、`enc_us`、`tx_us`）**：後端 internal 槽＋DMA，同 Wi-Fi／USB 爭 internal；
  `bus_ok=1` 只代表波形正確，唔代表冇被拖慢。
- **開機時間**：schema／資產首次讀 flash 會同 code fetch 爭 — 所以開機數字要同「第二次開機」分開。

## 4. 統計規矩（寫進測試流程）

1. 每次量測記錄 **where（S／D／P）**＋同場並行模組（Wi-Fi、MSC、pixel producer）。
2. **同一時間只量一個子系統**；要比較就同一塊板、同一個 ring policy、同一個 px。
3. PSRAM 類量測（reply／大環）要標明「有冇同時跑 pixel producer」。
4. 量 RTT 要扣掉 `0xF061`／`0xF081` 自身的 PSRAM 寫＋USB 傳輸；用 min／median，唔用平均。
5. 位置改動（例如 reply 搬 internal）要 A/B 同一日、同一映像，否則唔可比。

## 5. 未做（下一步可以量）

- **RTT：pixel producer 開／關** 同一支探針（估 PSRAM 爭用影響）。
- **FPS：Wi-Fi 開／關**（internal 與 cache 影響）。
- **reply0 搬 internal** 的 A/B（2,615 B × decoder 數）看 RTT 差幾多。
- 統計本身做成 module（`.nct`），用 name-hash 動態 key 存結果，避免污染 production。
