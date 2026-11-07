# 硬體設定的三層結構（preset → override → merged）

> 目標：**加一顆新 IC 只加一行資料，邏輯不變**；設定壞了有統一的處理；解析結果放
> PSRAM（低重要、少改動、少讀取）。

## 一、三層

```
① preset（const，放 flash / .rodata）
   config.c 裡的表：每個「已知 IC」一筆，出廠就有。
   例：ST7789 / GT1151Q / IP101 / SDIO(4-bit 40 MHz) / WS2812 …

        ↓  被 ② 覆蓋（逐欄位）

② override（使用者上傳的 JSON，住 /data/config/*.json）
   只寫「同 preset 唔同」的欄位；冇寫 = 用 preset。

        ↓  合併（一次，boot 時）

③ merged（heap，PSRAM，boot 後**唯讀**）
   nc_cfg_hardware() 回傳的就是它。之後任何人只讀不寫。
```

**為什麼要三層**：現在 hardware.json 每個欄位都要寫齊（連 60 行的 `_comment`），
而大部分值其實是「這顆 IC 的常識」。preset 把常識內建，使用者只寫差異。

## 二、preset 的資料形狀

```c
// 一個 preset = 「某種東西的某個型號」的完整預設值
typedef struct {
    const char *kind;      // "tft" / "touch" / "phy" / "sdio" / "led" …
    const char *name;      // "ST7789" / "GT1151Q" / "IP101" …
    const char *json;      // 該型號的預設（JSON 字串，同 hardware.json 同一個 schema）
} nc_hw_preset_t;

static const nc_hw_preset_t k_hw_presets[] = {
    { "tft",   "ST7789",  "{\"width\":240,\"height\":320,\"color_order\":\"RGB\",…}" },
    { "tft",   "ILI9341", "{\"width\":240,\"height\":320,\"color_order\":\"BGR\",…}" },
    { "touch", "GT1151Q", "{\"addr\":\"0x14\",\"int\":-1,…}" },
    { "phy",   "IP101",   "{\"addr\":1,\"autoneg\":1}" },
    { "sdio",  "default", "{\"freq\":40000000,\"width\":4}" },
};
```

- 全部是 `const char *` → 放 flash，不佔 RAM。
- 一個 preset 就是一段 JSON：**不需要為每顆 IC 發明一套 C struct**，schema 只有一份。
- 加新 IC = 表加一行，`config.c` 以外零改動。

## 三、合併規則（寫一次，永不改）

```c
// 使用者 JSON 逐欄位覆蓋 preset；缺席 = 保留 preset；型別錯 = log + 保留 preset。
static void merge_obj(cJSON *dst, const cJSON *src, nc_cfg_stat_t *st);
```

| 情況 | 行為 | 計數 |
|---|---|---|
| 使用者有寫、型別對 | 覆蓋 | — |
| 使用者沒寫 | 保留 preset | `defaulted++` |
| 使用者寫了但型別錯 / 超範圍 | **保留 preset** + `ESP_LOGW` 一次 | `invalid++` |
| 使用者寫了未知欄位 | 保留 preset + log | `unknown++` |
| preset 也沒有（新型號） | 用「全 0／安全值」+ log | `missing++` |

**重點**：這五種情況對所有 IC 都一樣。加 IC 不需要碰這段。

## 四、失敗政策（與 boot report 對齊）

每個硬體能力在 boot report 裡有一行三格（`want` / `found` / `started`），
而 `want` 的來源就是 merged config：

```
want    = merged.enable
found   = 探測結果（init 成功？）
started = main 有冇起
```

三種「沒起來」在 report 上一眼分得清：

| report | 意思 | log 長什麼樣 |
|---|---|---|
| want=0 | config 沒開 | `hw: sdio 未啟用（want=0）` |
| want=1, found=0 | config 開了但硬體不在／init 失敗 | `hw: sdio init 失敗（0x107）` |
| want=1, found=1, started=0 | 硬體在但 main 沒起（資源不足／被政策擋） | `sd: 掛載跳過（記憶體不足）` |

## 五、放 PSRAM

```c
// boot 時一次配置，之後唯讀（同 action table 一樣：統計 → 一次配置 → 封存）
typedef struct {
    nc_system_cfg_t   sys;
    nc_tasks_cfg_t    tasks;
    nc_hardware_cfg_t hw;
} nc_cfg_arena_t;

static nc_cfg_arena_t *s_arena;     // heap_caps_malloc(MALLOC_CAP_SPIRAM)
```

- 一份 arena，三個 struct 連續放 → **一塊、不碎片**。
- `nc_cfg_system()` 等回傳 `&s_arena->sys`，呼叫端完全不用改。
- 估算約 2 KB（system ~550、hw ~600、tasks ~700），省的是內部 RAM。
- 讀取頻率：開機一次 + STATUS 偶爾；PSRAM 快取足夠。

## 六、使用者「上傳自己的」的流程

```
出廠：/data/config/hardware.json = 最簡（只有 enable + 型號）
      {"SDIO":{"enable":1},"TFT":{"enable":1,"driver":"ST7789"}}
                ↓
boot：preset["sdio/default"] + preset["tft/ST7789"] ← 使用者 JSON 覆蓋
                ↓
merged：完整、可直接餵給 driver init 的設定
```

使用者要改（例如 SD 用 1-bit）就只寫 `{"SDIO":{"enable":1,"width":1}}`，
其餘（freq、腳位）仍然由 preset 補。

## 七、實作順序（每步可獨立驗證）

1. **arena → PSRAM**（不改語義，只改放置）＋ `STATUS cfg` 顯示 bytes/來源。
2. **preset 表 + merge**：先把 SDIO 一種搬過去，驗證「只寫 enable 都開得成」。
3. 其餘（TFT / touch / PHY / LED）逐類搬；每次加一行。
4. boot report 的 `want` 改讀 merged config（本來就是），`found`/`started` 在啟動點填。

## 八、與 boot/main 的關係

- **boot**：讀 preset + 使用者 JSON → merge → 填 report 的 `want` → 探測 → 填 `found`。
- **main**：只看 `found`，啟動服務 → 填 `started`。
- 之後 runtime 完全唔會再碰 config（`merged` 唯讀）。
