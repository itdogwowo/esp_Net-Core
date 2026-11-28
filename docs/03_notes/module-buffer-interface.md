# 模組緩衝介面：申請 → 分區 → 說明 → 產出

> 2026-11-28 用家定案（取代「manifest 寫死 px/fps」）：
> 「一開始告訴系統我要申請多大的輸入緩衝，然後為這個緩衝分區，説明這些緩衝分別
> 代表什麼；輸出緩衝也一樣。過去生產者消費者模式，可以向緩衝管理器申請。」

## 1. 原則

- **模組 = 生產者**；控制器／配對層 = **消費者**。中間透過緩衝管理器（`nc_buf`／arena）。
- 模組唔可以自己 malloc：要向 host 申請，host 由已 commit 的 arena 切。
- 每個 region 要**自描述**：方向（in／out）、大小、意思（name／type）。
- 方向固定：**in = 模組只讀；out = 模組只寫**；越界明示拒絕。

## 2. 三階段初始化（對照 NC4 自描述）

```text
① declare   guest 寫「我要什麼」：N 個 region {dir, bytes, name_id, type}
② bind      host 由 arena/nc_buf 切，寫回實際位置（app offset），唔夠 → 明示失敗
③ init      guest 拿到最終表，記住每個 region 的 offset，開始生產
```

- 表住 guest 線性記憶體（descriptor 之後、scratch 之前留位，或者 scratch 頭）。
- `px` 呢類**輸入參數**由控制器寫入 in region；模組用它自己算 out 要幾大 →
  在 ① 就已經回報（host ② 照切）。
- 唔夠記憶體：**唔好縮**、唔好靜默改用細 buffer —— 回錯誤、標 bad，status 見得到。

## 3. 對應現有 ABI v0

| 現況 | 目標 |
|---|---|
| descriptor 只有 `fb_off/fb_bytes`（host 用 manifest px 寫死） | descriptor 加「region 表」：`{off, bytes}` × N，由 ① 決定 |
| `nce_render` 寫死一個 fb | 模組按 region 寫（可以多個 out：RGB 分三個、或 RGBW 一個） |
| `px` 在 manifest | `px` 係 **in region 的一個參數**，由控制器寫 |
| fps 在 manifest | **刪除**：生產者唔管節奏；消費者幾時讀就幾時讀 |

## 4. 生產者／消費者（pixel 管線）

```text
控制器（mode／配對層）
   │ 寫 in region（px、參數）
   ▼
.nce 插件 ──填滿──► out region（0–255）
   ▲                    │
   │ 讀（可停在哪格／跳到哪格）
   └──── 消費者（layout／effect task）◄──┘
```

- 消費者可以「讀到第 k 格停」或者「跳到第 m 格」，因為 out region 係**平坦陣列**。
- 生產者唔知道消費者幾時讀；重複格、跳格都係消費者決定。

## 5. 要改嘅位（未做）

1. `nc_ncd` manifest v2：換成 region 宣告（或由 guest 在 declare 回報，manifest 只留 identity）。
2. `nc_mod_abi.h`：加 region 表結構＋方向常數。
3. `nc_plugin`：open 流程加 declare → bind 兩步；arena 由 `nc_buf` 切。
4. `nc_px_nce`：改由 region 表拿 out 指標（唔再固定 `NC_PLUGIN_FB_OFF`）。
5. 文件：`ncm-format.md`、動畫 `nct-nce-flow.html` 第 ③④ 步。
