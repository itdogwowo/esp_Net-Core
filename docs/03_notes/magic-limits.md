# 神奇的上限／數字清單（2026-11-28 審核）

> 用家要求：**選擇權交用戶**，容量盡量由開機動態掃描決定；只有「線上的格式」同
> 「身份字串」可以固定。呢份係逐個 `#define ..._MAX` 嘅分類同建議。

## A. 協議／線格式 —— **固定，唔應該動態**

| 常數 | 值 | 為何固定 |
|---|---|---|
| `NC4_REPLY_JSON_MAX` | ~~2,600~~ → **`NC4_MAX_PAYLOAD - 2`**（2026-11-28 改） | 由 frame 容量推導，一個真相；reply 幀有 `_Static_assert <= NC4_MAX_FRAME` |
| `NCM_HDR_SIZE` / `NC_NCD_HDR_SIZE` | 48 / 40 | 容器格式；改 = 版本升級 |
| 12-bit 值域、`.ncc` magic／version | — | 存檔格式 |

## B. 身份／路徑字串 —— 固定（改要全域一起改）

`NC4_LINK_NAME_MAX` 16、`NC4_ROUTE_NAME_MAX` 16、`NC_CFG_NAME_MAX` 16、
`NC_MOD_NAME_MAX` 24、`NC_PLUGIN_NAME_MAX` 24、`NC_PX_NAME_MAX` 24、
`NC_CFG_TASK_NAME_MAX` 24、`NC_MOD_PATH_MAX` 80、`NC_META_*` 24/64。

> 呢批唔影響「用幾多個」，只影響「名幾長」；改佢們要同時改存檔／協議／工具。

## C. 用戶會撞的容量 —— **應該由掃描／設定決定**

| 常數（位置） | 值 | 限制咩 | 現況 | 建議 |
|---|---:|---|---|---|
| `NC_NCD_MAX_PX`／`NC_MOD_MAX_PIXELS` | ~~4096~~ → **65535** | `.nce` 輸出值數 | ✅ 已改（2026-11-28） | — |
| `NC_MOD_MAX_MODULES` | 8 | 同時載入模組數 | 固定 array | 掃描 count → arena 內動態 array |
| `NC_MOD_MAX_ENTRIES` | 16 | 掃描表 | 固定 | 同上（掃到幾多 allocate 幾多） |
| `NC_MOD_MAX_CMDS`／`NC_NCD_MAX_CMDS` | 8／32 | 模組宣告指令數 | 固定 | 按 manifest 加總 → 一次 alloc |
| `NC_PLUGIN_MAX` | 8 | 插件實例表 | 固定 | 同 modules（兩者應同一來源） |
| `NC_TASK_MAX` | 20 | 任務槽 | 固定 array | 開機 count（固定 8＋掃到 N）→ 動態 |
| `NC4_LINK_MAX` | 6 | 線 | 固定 | config 掃描決定 |
| `NC4_ROUTE_MAX`／`ROUTE_OUT_MAX` | 8／4 | 路由／一列出幾多 | 固定 | 同上 |
| `NC_BUF_MAX` | 32 | 同時 buffer | 固定 | 由 declare 加總決定 |
| `NC_HW_MAX`／`NC_HW_CLAIM_MAX` | 32／64 | 硬體節點／claim | 固定 | 由 hardware.json 掃描 |
| `NC_CFG_GPIO_MAX` | 54 | 腳號 | **唔應該動**（P4 上限） | per-chip 掃描 |
| `NC_PX_JOB_RANGE_MAX` | 4 | 一個 job 幾段 range | 固定 | 用户會撞 → 動態 |
| `NC_PX_HW_INV_MAX` | 8 | 反相表 | 固定 | 依 lane 數 |
| `NC_PX_CODEC_ZONES_MAX` | 16 | 分區 | 固定 | 依設定 |
| `NC_PX_LAYER` `GROUP_MAX`／`SEL_MAX` | 65,535／4M | 選擇器上限 | 數字上限 | 可接受（真係上限） |

## D. 已經動態化（照抄呢個做法）

- **資產／mode／map／show**：掃到幾多配幾多（`{n,cap,item*}`）。
- **program 段**：全域 segment pool，`seg_off`（2026-11-28）。
- **模組 arena**：`nc_mod_arena_bytes()` 由掃描結果計（0 個 = 0 B）。
- **State store**：開機掃 records 再 malloc。
- **plan**：服務自己 declare，plan 加總再 carve。
- **cache**：大小由實際檔案／PSRAM 決定，上限 0 = 不限。

## E. 掃描決定容量的標準做法

1. **pass 1 只 count**（讀 header／目錄，唔 allocate）。
2. 容量 = count（＋少量 reserve，寫明幾多）。
3. **pass 2 一次過 alloc**（PSRAM 優先），填滿。
4. 放唔落 → `skipped++`／`err_*`＋log，**唔好靜默截**（鐵律 9）。
5. 上限仍然要留一個「安全閥」（防壞檔／惡意 manifest），但應該同
   `mem_bytes`／線路容量對齊，唔好亂寫 4,096 呢類整數。

## F. 仲未動態化的高風險項（按撞到機會排）

1. `NC_MOD_MAX_MODULES` 8 + `NC_PLUGIN_MAX` 8（兩個 8 要一致）
2. `NC4_LINK_MAX` 6 / `NC4_ROUTE_MAX` 8（多線／多路由用户會撞）
3. `NC_TASK_MAX` 20（核心 8＋模組 8＋…）
4. `NC_PX_JOB_RANGE_MAX` 4（一個 job 4 段 range）
5. `NC_HW_MAX` 32 / claim 64
