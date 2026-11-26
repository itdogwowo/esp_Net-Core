# 狀態形態與激活機制：FAT／PSRAM／RAM 三種形態

> 2026-11-26。用家問：「儲存在 FAT 同 PSRAM 的形態是不是不一樣？我需要激活機制，
> 不需要全部一開始就放上去。」
>
> 答案：**是，三種形態刻意不同**；而且 **enable=0 的東西完全唔佔 RAM／GPIO／driver**，
> 只有「已激活」嘅 unit 才 allocate。以下係定案設計。

---

## 1. 三種形態（同一個事實，三種表示）

| 形態 | 位置 | 內容 | 誰用 |
|---|---|---|---|
| **A. JSON** | FAT `/data/config/*.json` | key **路徑名**＋完整值（人改） | 用戶／WebMaster |
| **B. NCS1** | FAT `/data/config/state.ncs` 或 NVS blob | sparse `(id varint, 值 varint)`＋CRC | 開機快速載入／落盤 |
| **C. RAM** | **internal RAM**（唔係 PSRAM） | `uint32_t val[NC_KEY_MAX]` ＋ valid bitmap | state manager／driver |

### 為什麼 C 放 internal 而唔係 PSRAM？

| 項目 | 大小 |
|---|---:|
| `val[380]`（u32 每 key） | 1,520 B |
| valid bitmap（380 bits） | 48 B |
| 合計 | **≈1.6 KB** |

380 個 key × 4 B 只係 1.5 KB → **internal RAM 完全負擔得起**，而且 driver 讀值快
（PSRAM 讀取 ~10× 慢，仲要 cache 同步）。**PSRAM 只留俾「已激活 unit 嘅大 buffer」**
（環、DMA 槽、canvas），唔關狀態值事。

> 對比：JSON 8.8 KB（FAT）、NCS1 ~160 B（sparse）、RAM 1.6 KB（固定、全 key 預留）。
> 三者用途唔同，唔係「同一份嘢搬三次」。

---

## 2. 激活機制：declared ≠ activated

每個 unit（spi／i80／led／pca9685…）有三個獨立概念：

```text
declared   ← schema 有呢個 unit（entry 存在，enable 可以係 0）
enabled    ← config 寫 enable=1（用戶想用）
activated  ← driver 真正 install、GPIO claim、buffer allocate、任務接上
```

**不變量**：`activated ⊆ enabled ⊆ declared`。

- `enable=0`：**零成本** —— 唔 claim GPIO、唔 allocate buffer、唔 install driver，
  RAM 只用 `val[]` 嗰 4 B（預設值）＋bitmap 1 bit。
- `enable=1`：開機時按**依賴順序**激活（bus 先、device 後）。
- 激活失敗（GPIO 衝突／bus 起唔到／記憶體不足）→ **明示 error**，該 unit 入
  `error` 狀態，其他 unit 照跑（唔好成個 boot 掛）。

### 激活狀態機

```text
off ──enable──► requested ──claim ok──► resources ──alloc ok──► ready ──► running
                    │                      │                      │
                    └──error（reported）───┴──────────────────────┘
release ←── 用戶 disable／reboot
```

每個 unit 一個 `nc_act_t`（~16 B）：
`{ state u8, err u8, deps u8[], n_deps, owner_task, buf_handle }`。

### 開機分階段（取代「B05 一次裝晒」）

| 階段 | 做咩 | 激活邊個 |
|---|---|---|
| B05-core | console／USB／flash／FAT | 固定核心（永遠） |
| B05-cfg | parse JSON → NCS1 → `val[]` | 全部 declared（只填值，**唔裝 driver**） |
| B05-act | 掃 `enabled=1` 嘅 unit，按 topo 排序 | 只 activate 呢批 |
| B05-defer | `enable=0`（或 `auto=0`） | **唔郁**，等 runtime 指令 |

### Runtime 激活（第二階段）

- NC4 新指令：`activate <unit>`／`deactivate <unit>`（`0x10xx` 域，唔入 `0xFxxx`）。
- `activate`：將 `enabled` 設 1 → 行同一條激活路徑 → 成功後 `running`；
  **唔使 reboot、唔使重 flash**。
- `deactivate`：停任務 → release buffer（回 arena）→ 放 GPIO bit → `off`。
- `status hw`：逐 unit 顯示 `declared/enabled/activated`＋錯誤碼。

### 依賴圖（自動排序，唔使手寫次序）

```text
pin ─┬─ spi ─┬─ tft
     │       ├─ sd(spi)
     │       └─ apa102
     ├─ i2c ─┬─ pca9685
     │       ├─ xl9555
     │       ├─ gt1151q
     │       └─ husb238
     ├─ i80 ─── led(ws2812)
     ├─ i2s ─── pcm5102
     └─ uart ── uart_motor
```

- 激活 `tft` 會自動先激活 `spi`（同 `pin`）；bus 已被別人激活就共用。
- `deactivate` 最後一個用家離開才真正釋放 bus（refcount）。

---

## 3. 記憶體帳（激活 vs 唔激活）

| unit | 唔激活 | 激活後（大約） |
|---|---:|---:|
| `hw.pin` | 4 B 值＋1 bit | 0（純 GPIO 設定） |
| `hw.i80` + `dev.led`（Matrix） | 4 B×32＋32 bit ＝ ~160 B | 環 `i80.prd` 320 B×3＋DMA 槽 320 B×3＋driver ≈ **3 KB** |
| `hw.spi`（未接裝置） | 4 B×19 | bus init ≈ 1 KB（IDF 內部） |
| `hw.spi` + `tft` | 同上 | DMA buffer＋panel IO ≈ 16–32 KB |
| `dev.pca9685` | 4 B×4 | ~100 B |

→ **enable=0 嘅硬件全部加埋都係幾 KB 級（只係 `val[]` 預留）**；真正大嘅 allocation
只會喺激活時發生，而且由 `nc_buf` arena 一次 carve（唔准 runtime 向 heap 要，鐵律）。

---

## 4. 同「FAT 改完 reboot 生效」嘅關係

| 情境 | 做法 |
|---|---|
| 平時改設定（GPIO／enable） | 改 JSON → reboot（現有流程，簡單可靠） |
| 開機時只裝 enabled=1 | 新機制，B05-act 只做呢批 |
| 想唔 reboot 即時開一個 unit | v2 `activate` 指令（要 live release／claim，工作量大啲） |
| 壞檔／enable 打架 | 明示 error＋回 L0（driver／hardware 已做） |

**建議次序**：先做「**只裝 enabled**＋狀態機＋`status hw`」（安全、即刻省 RAM）；
`activate/deactivate` 指令排後（有需要才做）。

---

## 5. 落地（併入 state-schema 計劃）

| 階段 | 內容 | 估時 |
|---|---|---|
| **A1** | `nc_act` 狀態機＋unit 表（380 key 對應 unit 列舉）＋B05-act 只裝 enabled | 1 日 |
| A2 | 依賴圖 topo 排序＋GPIO bitmap claim＋refcount | 0.5 日 |
| A3 | `status hw`（declared／enabled／activated／err） | 0.5 日 |
| A4（可選） | `activate`／`deactivate` runtime 指令 | 1–2 日 |

**風險**：現時 B05 假設「全部 driver 一早裝好」；改成分階段要逐個 unit 檢查
`on_start()` 依賴（例如 render 要 i80 已 ready）。用 feature flag 分階段切換，
每切一個 unit 上板驗證。
