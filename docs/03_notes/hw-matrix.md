# 硬件矩陣：所有支援硬件＋所有 IO 位置（上限可算）

> 2026-11-26。用家設計：
> 1. **JSON 列出所有支援硬件**，預設 `"enable": 0`（＝可選清單／快速選配）；
> 2. 每個硬件有固定「通道上限＋欄位」，用戶通常只填 **GPIO ＋少量數值**；
> 3. 硬件＋IO 位置列成矩陣 → **key 數有限** → 儲存上限**計得出**，唔使估；
> 4. GPIO 用 **48-bit bitmap**（bit0..47），enable 時佔 bit → 衝突檢查 O(1)。

---

## 1. 矩陣（356 keys，覆蓋現時所有支援硬件）

| unit | 通道上限 | 欄位 | keys | 備註 |
|---|---:|---:|---:|---|
| `sys.system` | 1 | 12 | 12 | version／frame_interval／log… |
| `hw.pin` | **48** | 3 | **144** | 全 GPIO 位置矩陣（GPIO↔用途） |
| `hw.pwm` | 8 | 2 | 16 | |
| `hw.uart` | 3 | 6 | 18 | |
| `hw.spi` | 2 | 7 | 14 | |
| `hw.i2c` | 2 | 3 | 6 | |
| `hw.i2s` | 1 | 5 | 5 | |
| `hw.sdio` | 1 | 8 | 8 | |
| `hw.i80` | 2 | 22 | 44 | 含 16 條 data 位置 |
| `hw.eth` | 1 | 8 | 8 | |
| `hw.enc` | 1 | 2 | 2 | |
| `dev.usb` | 2 | 4 | 8 | |
| `dev.led` | 2 | 10 | 20 | WS2812 |
| `dev.apa102` | 2 | 5 | 10 | |
| `dev.pca9685` | 2 | 4 | 8 | |
| `dev.tft` | 1 | 8 | 8 | |
| `dev.xl9555` | 1 | 4 | 4 | |
| `dev.gt1151q` | 1 | 3 | 3 | |
| `dev.husb238` | 1 | 3 | 3 | |
| `dev.pcm5102` | 1 | 2 | 2 | |
| `dev.uart_motor` | 2 | 3 | 6 | |
| `dev.sd` | 1 | 5 | 5 | |
| `dev.audio` | 1 | 2 | 2 | |
| **合計** | | | **356** | |

> 加新硬件＝加一個 unit（通道×欄位）；加新欄位＝+1 key/通道。
> **每加 1 個 key，worst-case 上限 +7 B**（id ≤2 + 值 ≤5）。

---

## 2. 儲存上限（唔使估，算式如下）

```text
NCS1 header = 12 B
per key     = varint(id ≤2 B) + varint(value ≤5 B) = ≤7 B
worst       = 12 + N_keys × 7
```

| 情境 | keys | 大小 |
|---|---:|---:|
| **全部 356 key 都非預設（worst-case）** | 356 | **2,504 B** |
| 典型：只開 I80 + LED | 32 | **236 B** |
| Matrix 實際（21 個改動 key） | 21 | **~160 B** |

- NVS 單 blob 上限 4 KB → **2,504 B OK**（仲有 36% 餘裕）。
- 未來增長規則：`N_keys ≤ (4096 − 12) / 7 ≈ 583`。超過就拆兩個 blob（hw／dev）或
  用 NCS1 的「同值壓縮」（連續同 id 前綴）。
- **sparse 只存改動值**：enable=0 嘅硬件完全唔寫 → 通常 <300 B。

原型：`modules/lab/state_schema_lab/hw_matrix_preview.py`（實跑數字同上）。

---

## 3. 模板 JSON（所有硬件 `enable: 0`）

由 schema 生成（唔手寫），示範：

```json
{
  "version": 1,
  "hw": {
    "spi": { "enable": 0, "max": 2, "fields": 7,
             "list": [ { "ch0": {"gpio": -1} }, { "ch1": {"gpio": -1} } ] },
    "i80": { "enable": 0, "max": 2, "fields": 22,
             "list": [ { "ch0": {"data": [], "wr": -1, "dc": -1, "cs": -1, "freq": 80000000} } ] }
  },
  "dev": {
    "led":    { "enable": 0, "max": 2, "fields": 10 },
    "pca9685":{ "enable": 0, "max": 2, "fields": 4 }
  }
}
```

- 生成大小約 **5.1 KB**（356 keys 全部列出、enable=0）——放 FAT 只係模板，
  人睇／改；**儲存唔用呢個**（用 NCS1 sparse blob）。
- 用戶流程：開模板 → 將要用嘅 unit `enable` 改 1 → 填 GPIO／少數值 → reboot。
- 未知／未支援 unit 名 → parse 明示 warn（唔靜默）。

---

## 4. GPIO 矩陣（48 位 bitmap）

```c
uint64_t gpio_used;     // bit0..47；GPIO48 不存在，bit48 不用
uint64_t gpio_reserved; // flash 26..37、USB 19/20、console 43/44、strapping 0/3/45/46
```

- `enable=1` 時：所有宣告 GPIO（data／wr／dc／cs／sck／mosi…）→ 佔 bit；
  任何 bit 已被佔（或 reserved）→ **即時衝突，parse error**（現時係逐個 scan）。
- 好處：衝突檢查由「掃 356 key」變 **O(1)**；`status` 可直接印 bitmap／free mask。
- Matrix 示例：I80 D0=14、wr=1、dc=2 → `0x4006`（3 支）；
  再要 GPIO14 → 衝突；再要 GPIO10 → 可以。

---

## 5. 同現有 config 層關係

```text
L0 出廠：schema 表（356 key，型別/範圍/default）＋ 模板（enable=0）
L1 FAT： hardware.json／driver.json（人改；路徑名 → schema id）
L2 狀態： NCS1 blob（sparse，只存改動 key）
壞檔：   明示 warn → 回 L0（唔會靜靜降級）
```

- 儲存**上限已知**（2,504 B worst、236 B typical）→ 可以預先配置 NVS／RAM，唔會
  「唔知幾時爆」。
- schema ver 一改 → 舊 NCS1 blob 拒讀、由 JSON／L0 重建。

---

## 6. 落地（併入 state-schema 計劃）

| 階段 | 內容 | 估時 |
|---|---|---|
| P1 | `hw-matrix.json`（356 key）＋ncportgen 編 `nc_hw_matrix.c`（表＋容量）＋模板生成 | +0.5 日 |
| P2 | NCS1 codec＋`nc_state` v2（含 worst-case 靜態檢查：356×7+12 ≤ 4 KB assert） | 1 日 |
| P3 | JSON parser 用矩陣（路徑→id）＋GPIO bitmap claim | 1 日 |
| P4 | `status cfg`：blob size／key 數／bitmap／schema ver | 0.5 日 |

**新增不變量**：編譯期 `static_assert(12 + NC_HW_MATRIX_KEYS*7 <= 4096)` —— 上限寫死在
code 裡，加硬件／欄位時一超出就 build 失敗（唔會 runtime 先爆）。
