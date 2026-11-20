# 行為分類與落位：三平面模型（CORE／USER／DATA）

> **用途**：任何新東西（驅動、效果、任務、指令、資料）加入之前，先用這份定「角色、平面、家、名」。
> **狀態**：**設計階段（2026-11-19）**。本文只定規則與落點，未實作；實作前先答 §8 的待定問題。
> **參考**：`mp_Net-Core/slave/`（MicroPython 版）—— 佢個架構係本文的對照基準。
> **相關**：[component-layout.md](component-layout.md)（CORE 內部佈局）· [naming.md](naming.md)（命名）·
> [writing-a-task.md](writing-a-task.md)（task）· [effect-language.md](effect-language.md)（`.nce` ABI）·
> [extension-architecture.md](extension-architecture.md)（`kind` 四種）· [config-layers.md](config-layers.md)（DATA 設定）

---

## 0. 一句話

**MP 只需要一個軸：角色。C 需要兩個軸：角色 × 平面。**

| | MP（MicroPython） | C（本專案） |
|---|---|---|
| 什麼是程式 | 檔案（`.py` / `.json`） | 編譯後的機器碼 |
| 核心 vs 用戶 | **冇分別**：全部係 user space | **有分別**：燒入 flash 的 core vs 可換的 user |
| 邊界靠乜 | 慣例（目錄、registry） | 慣例 **＋** 編譯邊界、記憶體契約、ABI |
| 換效果 | 改 JSON／丟 `.py`，下一幀生效 | 三條路：改 DATA／重燒 CORE／丟 `.nce` |

所以 C 的規則係：

> **角色決定家（住哪個 component），平面決定圍欄（可以碰什麼），契約決定兩邊點見面。**

---

## 1. 先看 MP：一個軸就夠

### 1.1 目錄按角色，唔按功能

```text
mp_Net-Core/slave/
├─ app.py / boot.py / Core_Manager.py   # 裝配、硬體初始化、調度
├─ driver/            # 硬體：ws2812_drv / tft_drv / i2c_drv …（只知自己的周邊）
├─ lib/
│  ├─ hw/             # 具體晶片：TFT.py / pca9685.py / xl9555.py
│  ├─ sw/             # 純軟件：effect_core / PixelMathMethod / pixel_layout / PixelController
│  └─ sys/            # 系統：config / bus / proto / dispatch / task_manager / schema_codec …
├─ action/            # 指令政策：<域>_actions.py + registry.py
├─ tasks/             # 迴圈：network / render / pixel_task / bus_decode …
├─ schema/            # 協議契約：<group>.json（cmd + payload）
├─ pixel/             # ★ 一個「域」的資料樹：effects / map / modes / registry.json
└─ ui/                # 頁面
```

三個特徵：

1. **角色目錄**：`driver` 只知硬體、`action` 只做指令、`tasks` 只管迴圈、`lib/sw` 只放演算法。
2. **`pixel/` 係域，唔係角色**：它裝的是「效果、mapping、mode、播放清單」四層**資料**，程式部分（框架、layout、數學）在 `lib/sw/`。
3. **冇 core/user 邊界**：所有 `.py` 都係檔案，改完 import 就生效；壞效果最多 soft reset。

### 1.2 為什麼 MP 唔需要分層

Python 全部係 user space：冇特權指令、冇直接記憶體指標（大部分情況）、一個模組改壞唔會 corrupt kernel。
所以 MP 只需要回答「這個檔案係邊個角色」，唔需要回答「佢有冇特權」。

### 1.3 MP 的四個習慣（C 要繼承）

| 習慣 | MP 的做法 | 為什麼重要 |
|---|---|---|
| **資料先於程式** | 效果優先用 `effects.json` 的 `program` 畫波；畫波寫不出來才寫 `.py` | 八成效果唔需要程式碼；改資料唔使重燒 |
| **框架與內容分離** | `lib/sw/effect_core.py` 只有 `Effect` 基類／登記表／波表；具體效果一律在 `pixel/effects/` | 框架唔會愈養愈肥，內容可以獨立換 |
| **計算與播送分離** | `pixel_task`（算 + scatter）→ hub → `render.py`（固定 fps 推硬件） | 效果節奏同送燈節奏解耦 |
| **檔案系統就是註冊表** | `effects.json` / `map/*.json` / `modes/*.json` / `registry.json`；`ls` 就知有咩 | 唔使改程式加效果，唔使重編 |

### 1.4 MP 的 pixel：四層資料 ＋ 三種寫法

```text
effects/   （怎麼動）    effects.json（program 波形 + 參數）＋ effects.py（畫波寫不出來的類別）
map/       （怎麼排）    map/*.json（群組 → 有序段選擇，可交叉型別／反序／重疊）
modes/     （點配對）    modes/*.json（效果 × 群組 + write + range + 播放參數）
registry.json（播什麼）  auto_play + list（大隊列）
```

效果本身有三種寫法，**由簡到繁**：

| 路 | 寫什麼 | 放哪 | 什麼時候用 |
|---|---|---|---|
| **A 畫波** | 純 JSON `program`（`keep` / `math_now` / `square_wave_now` / `pulse_wave` …） | `effects.json` | 首選；八成效果 |
| **B 自訂 frame** | `class xxx(Effect)` + override `frame(t)` | `effects.py` | 空間分布／狀態機，畫波寫不出來 |
| **C 完全自訂** | 不繼承，實作 `__next__` / `restart` / `seek` / `release` | `effects.py` | 最後手段 |

而 **mapping / modes / playlist 的內容全部係 JSON**；處理它們的引擎在 `lib/sw/`（`pixel_layout.py`），唔在資料樹裡面。

### 1.5 MP 一幀的生命週期

```text
effects.json ──► effect_core（Effect 基類 + 波表）
                    │  make(name) → generator
                    ▼
pixel_task（core1）：playlist → mode → generator → array('H') 畫布
                    │  scatter（pixel_layout：群組選擇 → 落點）→ 寫入 hub slot
                    ▼
              hub（pixel_stream，多槽；滿即 drop，同一幀重試）
                    ▼
              render.py（core0）：read_into(big_buffer) → PixelController.show_all()
                    ▼
              WS2812 / APA102 / PCA9685 / 馬達
```

---

## 2. C 多了什麼：三個平面

### 2.0 詞彙：三個唔同嘅嘢，唔用同一個字

「效果」呢兩個字在本專案有三種意思，**落位之前先認清手上係邊一種**：

| 名 | 係咩 | 住邊 | 例 | 判準 |
|---|---|---|---|---|
| **引擎**（engine／工具） | 提供能力：契約、registry、program interpreter、波表快取、layout | `sw/pixel/engine/` | `include/nc_px_engine.h`、`nc_px_engine_core.c`、`nc_px_engine_program.c` | 拿掉所有實作，它依然可編、可測、可跑 |
| **實作**（implementation／內容） | 用引擎做出嚟嘅具體效果 | `components/effects/<域>_effects/` | `pixel_effects/nc_effect_breath.c` | 加／刪一個效果，引擎一行都唔使改 |
| **呼叫方**（caller） | 用引擎嘅節奏、指令、載入器 | `tasks/`／`action/`／`host/` | `pixel_task`（show runner）、`pixel_action`、`host/ncm` | 有核、有 fps、有生命週期 |

> **一條規則**：`sw/` **只放工具**。任何「用工具做出嚟嘅嘢」（效果實作、task、action）都唔准住 `sw/`。
> 命名亦分開：引擎 `nc_px_engine_*`（`sw/pixel/engine/`）；實作 `nc_effect_<name>.c`（`effects/`）。見 §6。

### 2.1 三個平面

| 平面 | 住哪 | 係咩 | 換一次要 | 誰可以改 |
|---|---|---|---|---|
| **CORE** | `firmware/components/` | 編譯入 flash：協議、驅動、環／編碼器、框架、內建效果、任務、指令 | 重燒／OTA | 開發者 |
| **USER** | repo 根 `modules/` → 裝置 FAT | 可載入：`.nce` 效果、`.nct` 任務／指令 | 丟檔案 | 使用者／第三方 |
| **DATA** | `ports/project/.../data/` → 裝置 FAT `/data/` | 設定與資產：`config/*.json`、`pixel/{effects,map,modes}/`、`registry.json` | 改檔 | 使用者 |

> **CORE 不是「比較重要的 code」**，係「有特權、開機必需、改動要重燒」的 code。
> **USER 不是「比較不重要」**，係「可以換、有沙箱、壞咗唔會燒板」的 code。
> **DATA 不是「設定而已」**，係「唔執行、只宣告」的內容；它是 MP 的「檔案系統就是註冊表」在 C 的落點。

### 2.2 邊界由四件事決定

同一個功能，放 CORE 定 USER，問四條：

| 問題 | 是 → CORE | 否 → USER |
|---|---|---|
| 佢碰硬件暫存器／DMA／中斷？ | ✅ | ❌ |
| 佢開機就要存在（冇佢開唔到機）？ | ✅ | ❌ |
| 佢要直接配置記憶體／接管環？ | ✅ | ❌ |
| 佢改一次可以接受重燒？ | ✅ | ❌ |

四條都「否」→ 佢係 USER 或 DATA 的材料。
**任何時候都唔准**：CORE 依賴某一個 USER 模組；USER 直接呼叫 driver；DATA 入面有可執行邏輯。

### 2.3 平面規則（硬規則）

| # | 規則 | 一句話 |
|---|---|---|
| P1 | **CORE 唔准 include `modules/`** | 核心唔知道有邊個插件；只經契約 |
| P2 | **USER 只可以 include 契約 header** | 例如 `nc_mod_abi.h`；唔可以 include driver／task／環 |
| P3 | **USER 唔准碰硬件** | 要 GPIO 只可以經 whitelist import（`nc_gpio_*`） |
| P4 | **DATA 唔准有可執行邏輯** | 唯一例外：宣告式 `program`（由 CORE interpreter 有界解釋） |
| P5 | **契約只有一份** | 效果＝canonical frame；任務＝`nc_task_t`／manifest；指令＝cmd schema |
| P6 | **平面各有註冊表** | CORE＝constructor／表；USER＝目錄掃描＋manifest；DATA＝檔案系統 |

### 2.4 角色 × 平面（權威表）

| 角色 | CORE（`firmware/components/`） | USER（`modules/`） | DATA（`/data/`） |
|---|---|---|---|
| **driver** | `driver/drv_*` | ❌ 永遠唔准 | `hardware.json`（值） |
| **hw**（晶片） | `hw/<chip>/` | ❌ 永遠唔准 | `hardware.json`（值） |
| **bus** | `bus/*` | ❌ 永遠唔准 | — |
| **sys** | `sys/*` | ❌ 永遠唔准 | `config/*.json` |
| **sw**（工具） | `sw/pixel/`（`ring/`·`encode/`·`dither/`·**`engine/`**＝效果引擎 `nc_px_engine_*`） | ❌ 永遠唔准 | — |
| **effect**（實作） | `effects/<域>_effects/`（內建效果，`nc_effect_*.c`） | `modules/effects/<name>_effects/` → `.nce` | `pixel/effects/*.json`（program）、`pixel/map/`、`pixel/modes/`、`pixel/registry.json` |
| **task** | `tasks/*_task/` | `modules/tasks/<name>_tasks/` → `.nct` | `config/tasks.json`（開關／核心） |
| **action** | `action/*_action/` | `modules/actions/<name>_actions/` → `.nct`（`cmds`） | `schema/*.json`（build 時生成，唔入 FAT） |
| **host** | `host/ncm/`（loader＋WAMR） | ❌ 永遠唔准 | — |
| **ui** | `ui/*`（未落地） | 未來可做 view 插件 | `ui.json`（未落地） |
| **schema** | 生成物 | ❌ | 來源 JSON（build 時） |

六個「❌ 永遠唔准」的角色：**driver、hw、bus、sys、sw、host** —— 佢哋碰硬件、記憶體、載入器；
（`schema` 係 build 生成物，唔算落 USER 的角色。）
放去 USER 等於將特權交給檔案。**USER 只准四個角色：effect、task、action、ui。**

---

## 3. 效果器：一個角色、三個交付、四層資料

### 3.1 為什麼要三種交付

MP 只有一條路（全部 Python），C 要按「風險 × 成本」分三級：

| 交付 | 住哪 | 重燒 | 速度 | 風險 | 誰寫 | 改一次 |
|---|---|---|---|---|---|---|
| **DATA program**（JSON 波形） | `/data/pixel/effects/*.json` | ❌ | 中（interpreter） | 最低：冇指標、冇迴圈，interpreter 有界 | 使用者 | 改檔 |
| **CORE built-in**（C） | `components/effects/<域>_effects/` | ✅ | 最高 | 中：要 review；寫錯指標可以 corrupt | 開發者 | 重燒 |
| **USER `.nce`**（wasm） | `modules/effects/` → FAT | ❌ | 較慢（直譯） | 最低：沙箱、線性記憶體、無指標 | 使用者／第三方 | 丟檔 |

> **順序就係優先順序**：能寫成 DATA program 就唔好寫 C；能寫 C 就唔好寫 wasm。
> 呢個正好對應 MP 的路 A／B／C —— 只係 C 版將「B／C」擺去兩個唔同平面。

### 3.2 四層資料（MP → C 落點）

| 層 | MP | C 平面 | C 位置 | 現況 |
|---|---|---|---|---|
| **effect**（怎麼動） | `pixel/effects/effects.json` + `.py` | DATA＋CORE＋USER | `/data/pixel/effects/*.json`；`components/effects/`；`modules/effects/` | 🟡 只有 CORE 內建 C，硬編碼 64 px |
| **mapping**（怎麼排） | `pixel/map/*.json` + `lib/sw/pixel_layout.py` | DATA＋CORE | `/data/pixel/map/*.json`；`sw/pixel/layout` | ❌ 冇（8×8 寫死） |
| **mode**（點配對） | `pixel/modes/*.json` | DATA＋CORE | `/data/pixel/modes/*.json`；`sw/pixel/mode` | ❌ 冇（一次一個全局效果） |
| **playlist**（播什麼） | `pixel/registry.json` | DATA＋CORE | `/data/pixel/registry.json`；`tasks/pixel_task`（show runner） | ❌ 冇 |
| **引擎** | `lib/sw/effect_core.py` | CORE（sw） | `sw/pixel/engine/`（`nc_px_engine_core` / `nc_px_engine_program`） | 🟡 同 producer 混在一起 |
| **計算** | `tasks/pixel_task.py` | CORE | `tasks/pixel_task/` | 🟡 有，但只係「算一格寫環」 |
| **播送** | `tasks/render.py` | CORE | `tasks/render_task/` | ✅ |
| **指令** | `action/pixel_actions.py` | CORE | `action/pixel_action/` | 🟡 只在 `test_action`（dev） |

**四層的職責一句話**：

- effect：**怎麼動**（時間 → 一條值）
- mapping：**怎麼排**（值 → 哪顆燈；群組、反序、重疊、跨型別）
- mode：**點配對**（哪個效果放哪個群組、哪個通道、播幾次）
- playlist：**播什麼**（順序、auto_play）

### 3.3 引擎 vs 實作（MP 的規矩，C 照跟）

| | 引擎（工具：CORE `sw/pixel/engine/`） | 實作（內容：CORE `effects/`／USER modules／DATA） |
|---|---|---|
| 有咩 | `nc_px_engine_desc_t`、registry、program interpreter、波表快取、衝突檢查 | 具體效果（呼吸／波浪／文字／火焰） |
| 改一次 | 重燒（少） | 改 DATA 或丟 `.nce`（多） |
| 唔准 | **唔准包含任何一個具體效果** | **唔准自己開 task／寫環／碰 driver** |

MP 的 `effect_core.py` 開頭明寫：「具體效果一律放 `slave/pixel/effects/`，本模組不寫任何效果。」
C 版一樣：`sw/pixel/engine/` 唔准出現 `breath` / `wave` 呢啲名；實作一律住
`components/effects/pixel_effects/`（`nc_effect_*.c`），只 include 契約 header。

### 3.4 效果契約（唯一一份，三個交付共用）

```c
// firmware/components/sw/pixel/include/nc_px_engine.h（目標形狀；M3 凍結）
typedef struct {
    uint32_t px;          // 這塊畫布幾顆
    uint16_t w, h;        // 幾何；h == 1 表示一維鏈
    int64_t  now_us;      // 唯一時間來源
    const void *params;   // 效果參數（DATA／action 寫入，effect 只讀）
} nc_px_engine_in_t;

typedef struct {
    const char *name;
    uint32_t    state_bytes;   // 0 = 無狀態；>0 由 caller 提供
    bool (*render)(void *st, const nc_px_engine_in_t *in, uint8_t *dst);  // dst = px × 4（R,G,B,W）
} nc_px_engine_desc_t;
```

六條不變式（違反就係 bug）：

1. **純**：同一 `(st, in)` → 同一 `dst`；唔准 RNG、唔准讀可變全域。
2. **無配置**：唔准 `malloc`／`nc_buf`／`nc_plan`。
3. **無 IO**：唔准 `ESP_LOG`（熱路徑）、driver、bus、檔案系統。
4. **無排程**：唔准 FreeRTOS／`esp_timer_start`／開 task。
5. **有界**：一幀要在預算內完成；唔准無上限迴圈。
6. **寫滿**：`dst` 的 `px × 4` B 全部要寫。

**效果只畫「畫布」，唔做 mapping、唔知 lane、唔寫環** —— mapping 同 commit 係 CORE 的責任。

### 3.5 計算 vs 播送（MP 的雙核分工，C 已經有）

| | 計算（`tasks/pixel_task`） | 播送（`tasks/render_task`） |
|---|---|---|
| 做什麼 | playlist → mode → generator → 畫布 → scatter → 環 | 取最新格 → 編碼 → 後端 → DMA |
| 節奏 | 由 show／效果決定（唔綁 fps） | 由 `frame_interval_ms` 決定（固定 fps） |
| 見到效果嗎 | 係（擁有 effect 實例） | 唔係（只見到 canonical frame） |
| 可以停嗎 | 停效果 → 燈保持最後一格 | 停 render → 效果照寫環（滿了跳過） |

### 3.6 C 版一幀怎麼走（目標）

```text
/data/pixel/effects/*.json   /data/pixel/map/*.json   /data/pixel/modes/*.json   /data/pixel/registry.json
        │                          │                         │                          │
        ▼                          ▼                         ▼                          ▼
  nc_px_engine_program        nc_px_layout              nc_px_mode                playlist
  （波形 → 畫布）              （群組 → 全域 index）       （效果 × 群組 + 參數）      （播什麼）
        └──────────────┬───────────┴─────────────┬───────────────┘
                       ▼                         ▼
              tasks/pixel_task（show runner）：generator → 畫布 → scatter → 環（canonical RGBW）
                       │
                       ▼
              tasks/render_task：peek → encode → backend（i80／spi／direct／mock）→ DMA
                       │
                       ▼
              driver/drv_i80、drv_spi_led
```

---

## 4. 決策樹（角色 → 平面 → 家）

```text
新嘅嘢係…
├─ 直接寫周邊暫存器／時序／腳位？ ────────────► CORE driver/drv_<hw>
├─ 用 bus 去操作一顆具體晶片（寄存器／封包）？ ─► CORE hw/<chip>
├─ 係一條線的統一讀寫語義？ ──────────────────► CORE bus/<bus_x>
├─ 協議／記憶體／設定／儲存／狀態／任務管理？ ─► CORE sys/<name>
├─ 只係工具（演算法／資料結構：環、編碼、layout、效果引擎）？ ► CORE sw/<name>
├─ 「時間／參數 → 畫面」的行為？ ─────────────► 先問交付：
│    ├─ 波形寫得出（keep / math / square / pulse）？ ► DATA program（/data/pixel/effects/*.json）
│    ├─ 開發者、要最快、可重燒？ ─────────────────► CORE effects/<域>_effects/
│    └─ 使用者／第三方、要沙箱、唔重燒？ ──────────► USER modules/effects/*.nce
├─ 有自己節奏／核／生命週期？ ────────────────► CORE tasks/<域>_task（或 USER .nct）
├─ 對外係一條指令？ ──────────────────────────► CORE action/<域>_action（或 USER .nct）
├─ 要載入並執行外部檔案？ ────────────────────► CORE host/<name>
└─ 都唔係？ → 停手。先寫返「輸入、輸出、時間觀、平面」四行，再返來問。
```

三個必答問題：

1. **佢有冇特權？**（碰硬件／記憶體／開機必要）→ 有就只可以 CORE。
2. **佢改一次要唔要重燒？** → 要就 CORE，唔要就 DATA／USER。
3. **佢係工具、實作、定係呼叫方？** → 工具（引擎）放 `sw/`；實作放 `effects/`／`modules/`／`data/`；呼叫方放 `tasks/`／`action/`／`host/`。

---

## 5. 依賴與圍欄

```text
CORE（firmware/components/）
  tasks/* ──► effects/* ──► sw/* ──► sys/* ──► bus/* ──► driver/*
     └──────► hw/*（用晶片）──► bus/* ──► driver/*
  action/* ──► effects/*（設參數）、sw/*、sys/*、hw/*（設晶片參數）
  host/* ───► sys/*（載入／隔離）；只交 canonical frame 出去
  ui/* ─────► sys/*（經指令）

USER（modules/）
  effects/*.nce ──► 只可以 import 契約白名單（nc_print / nc_gpio / nc_now）
  tasks/*.nct   ──► 同上
  actions/*.nct ──► 同上（cmds 由 manifest 宣告）

DATA（/data/）
  *.json ──► 由 CORE parser 讀；壞檔＝用預設／保留 last-good，唔可以擋開機
```

六條硬規則：

1. **CORE 箭頭只向下**：`tasks → effects → sw → sys → driver/bus`；唔准反向。
2. **CORE 唔准 include `modules/`**（P1）。
3. **USER 只可以 include 契約 header**，唔准 include driver／task／環（P2）。
4. **DATA 唔准有可執行邏輯**，唯一例外係宣告式 `program`（P4）。
5. **同層唔互相 include**；要共用就落 `sw/`。
6. **平面之間只傳資料**：CORE → USER 交 buffer／參數；USER → CORE 交 frame／回傳值。冇指標互換（wasm 線性記憶體）。

---

## 6. 命名

| 東西 | 規則 | 例 |
|---|---|---|
| CORE 晶片 | `components/hw/<chip>/` | `pca9685`（`nc_pca9685.c`） |
| CORE 效果 component | `components/effects/<域>_effects/` | `pixel_effects` |
| CORE 效果檔（實作） | `nc_effect_<name>.c`（住 `<域>_effects/`，域由 component 名講） | `pixel_effects/nc_effect_breath.c` |
| 效果契約 | `sw/pixel/include/nc_px_engine.h` | `nc_px_engine_desc_t` / `nc_px_engine_in_t` |
| 效果引擎（工具） | `sw/pixel/engine/nc_px_engine_core.c` / `nc_px_engine_program.c` | registry、波表、interpreter |
| 效果符號 | 引擎 `nc_px_engine_*`；實作 `nc_effect_<name>` | `nc_px_engine_register()`、`nc_effect_breath` |
| USER 效果 | `modules/effects/<name>_effects/` → `out/<name>.nce` | `fire_effects` |
| DATA 效果檔 | `/data/pixel/effects/<name>.json` | `breathing.json` |
| DATA mapping | `/data/pixel/map/<name>.json` | `matrix.json` |
| DATA mode | `/data/pixel/modes/<name>.json` | `demo_eyes.json` |
| DATA playlist | `/data/pixel/registry.json` | `{"auto_play":true,"list":[...]}` |
| task／action | `<域>_task` / `<域>_action` | `pixel_task` / `pixel_action` |

> ⚠️ 兩種 `_effects` 的前綴語義唔同：CORE 係 `<畫布域>_effects`（一個 component 裝好多效果）；
> USER 係 `<模組名>_effects`（一個容器一個效果）。後綴一樣、前綴唔可以照抄。

---

## 7. 現況 vs MP（差距表）

| 能力 | MP | C 現況 | 影響 |
|---|---|---|---|
| 晶片層（`lib/hw`） | `lib/hw/TFT.py`、`pca9685.py`、`xl9555.py` | ❌ `components/hw/` 未落地（`drv_spi_led` 暫時兼任燈種） | 第二顆晶片出現時冇家 |
| 效果框架／內容分離 | `effect_core.py` vs `pixel/effects/` | producer 一個檔做效果＋抖動＋demo＋參數 | 加效果要改 C、要重燒 |
| DATA program（路 A） | `effects.json` 的 `program` | ❌ 冇 | 每個效果都係 C 代碼 |
| 內建效果（路 B/C） | `effects.py` | 🟡 有 6 個硬編碼效果 | — |
| USER `.nce` | —（MP 全部係 user space，唔需要） | 🟡 有容器／WAMR，但 **framebuffer 冇人讀** | 外掛效果亮唔到燈 |
| mapping | `map/*.json` + `pixel_layout.py` | ❌ 冇；8×8／64 px 寫死 | 非 8 寬幾何、>64 px 全部黑 |
| modes | `modes/*.json` | ❌ 冇 | 一次只可以一個全局效果 |
| playlist | `registry.json` | ❌ 冇 | 冇自動播放／輪播 |
| 計算／播送分離 | `pixel_task` / `render.py` | ✅ 已有 | — |
| 指令 | `pixel_actions.py` | 🟡 只在 `test_action`（`NC_ENABLE_TESTS`） | release 映像改唔到效果 |
| 檔案系統就是註冊表 | ✅ | 🟡 `/data/pixel/effects/` 有掃，但只載入唔使用 | 丟檔冇反應 |

**一句話**：C 版有 MP 的「骨架」（環、渲染、任務），但缺 MP 的「內容層」（四層資料）同「框架／內容分離」。

---

## 8. 設計決定

### 8.1 已定（本輪）

| # | 決定 | 理由 |
|---|---|---|
| D1 | 三個平面：CORE／USER／DATA | C 有編譯邊界；MP 的一個軸唔夠 |
| D2 | 角色表＋平面表＝權威 | 一個功能會被拆成幾個家；唔准按功能放 |
| D3 | USER 只准 effect／task／action／ui 四個角色 | 其餘角色碰特權，放 USER 等於開後門 |
| D4 | 效果**引擎**（工具）住 `sw/pixel/engine/`，**實作**住 `effects/<域>_effects/` | 跟 MP 的 `lib/sw/effect_core.py` vs `pixel/effects/`；`sw/` 只放工具 |
| D5 | 效果三種交付：DATA program／CORE C／USER `.nce`，同一契約 | 風險 × 成本分級；MP 的路 A/B/C 對應 |
| D6 | 效果只畫畫布；mapping／scatter／commit 係 CORE | 效果唔知 lane／硬件，`.nce` 先可以跨平台 |
| D7 | 四層資料（effect／map／mode／playlist）全部先行設計 | MP 已驗證；C 缺呢四層就永遠係「一個全局效果」 |
| D8 | DATA 用檔案系統做註冊表，壞檔降級不擋開機 | 跟 MP；亦係現有 config 原則 |
| D9 | 計算（pixel_task）與播送（render_task）繼續分開 | 已有、MP 同構；效果節奏唔綁 fps |
| D10 | 平面之間只傳資料，唔傳指標 | wasm 線性記憶體、C ABI 邊界 |
| D11 | **引擎／實作／呼叫方三個名分開**：引擎＝`sw/pixel/engine/`（`nc_px_engine_*`）、實作＝`effects/`（`nc_effect_*`）、呼叫方＝`tasks/`／`action/`／`host/` | 「effects」一個字有兩個意思（工具＋實作）；唔分開就永遠擺錯位 |

### 8.2 待定（實作前要拍板）

| # | 問題 | 選項 | 建議 |
|---|---|---|---|
| Q1 | DATA program 的 schema：照搬 MP 的 `keep/math_now/square_wave_now/pulse_wave/pulse/starter`？ | 照搬／縮小／自定 | 先照搬（有現成 MP 效果可對照），C interpreter 另寫 |
| Q2 | mapping 的 schema：MP 的 `group + sel`（可交叉型別／反序／重疊）定係簡化？ | 全功能／v1 只做每鏈範圍／先 identity | v1 先做「每鏈範圍＋反序」，全功能後補 |
| Q3 | mode 的 `write`（r/g/b/w/rgb/rgbw）同 `range`（群組內切片）要唔要 v1 就有？ | 要／唔要 | 要 `rgb`／`range`；其餘後補 |
| Q4 | playlist 要唔要 MP 的 `play_loop`／`play_count`／`play_interval`？ | 全要／先 `auto_play+list` | 先 `auto_play+list`，其餘後補 |
| Q5 | JSON parser 放邊：重用 `sys/config` 的 cJSON？定 `sw/pixel` 自己一個？ | 重用／獨立 | 重用 `sys`，但 parser 放 `sw`（純函數、host 可測） |
| Q6 | `.nce` 效果輸出：canonical 畫布（CORE scatter）定自己 scatter？ | 畫布／自己 | 畫布（D6） |
| Q7 | program interpreter 要唔要「波表快取＋warm_up」（MP 的做法）？ | 要／唔要 | 要；開機預算，熱路徑零浮點 |
| Q8 | 內建 C 效果保留幾多？ | 全保留／只留診斷 | 只留診斷（off／zone1／ramp／solid），其餘搬 DATA |
| Q9 | 先做邊層？ | program／mapping／modes／playlist | 先 `program`（解硬編碼／64 px 限）＋`mapping`（解幾何） |
| Q10 | `data/pixel/` 第一版放邊：`ports/project/.../data/` 定 root `data/`？ | 專案預置／共用 | 跟現有 config：`ports/project/<board>/data/` |

---

## 9. 遷移路線（設計定案後才動手）

| 步 | 做什麼 | 產出 | 驗收 |
|---|---|---|---|
| **M1** | 本文＋三平面規則＋`components/effects/`＋README | 文件、目錄 | 文件連結零 broken；`git grep` 找得到規則 |
| **M2** ✅（2026-11-20） | 拆引擎／實作：效果數學 → `effects/pixel_effects/`（`nc_effect_*.c`）；引擎 → `sw/pixel/engine/`（`nc_px_engine_*`）。**demo＋參數面暫時仍住引擎**（0xF060 要讀 demo stage；搬去 task 要先改狀態通道）→ 留 M2b | CORE 兩層 | ✅ 8 塊板建置過、新檔 0 warning；✅ `fx_test` 15 個案例 × 15 幀 payload hash 同重構前逐 byte 一致 |
| **M3** | 效果契約 `nc_px_engine.h`（含 `nc_px_engine_desc_t`）；畫布由環 px 決定（取消 64 限） | 契約 header＋adapter | 20／64／100 顆鏈都出得畫面 |
| **M4** | DATA program：`/data/pixel/effects/*.json`＋interpreter＋波表 | 路 A | 加一個 JSON = 多一個效果，唔使重燒 |
| **M5** | mapping：`/data/pixel/map/*.json`＋layout engine（scatter） | 路 B | 非 8 寬／多鏈／反序都正確 |
| **M6** | modes＋playlist：配對、`range`、`auto_play` | 路 C | 一個畫面可以兩個效果／兩個區域 |
| **M7** | `.nce`：`fx_render()` 後把 guest framebuffer 抄成 canonical frame | USER 效果 | 丟一個 `.nce` 就亮 |
| **M8** | `action/pixel_action/`（release 可用）＋主機推送（`NC_PX_SRC_HOST`） | 指令面 | release 映像改得到效果 |

> M2–M3 係「結構」；M4–M6 係「MP 的內容層」；M7–M8 係「USER 平面」。可以分開 review、分開上板。
> M2 已落地（引擎／實作分家）；M3（契約凍結＋畫布由環 px 決定）未做。
> `components/hw/` 唔急：等第二顆具體晶片（TFT／PCA9685／XL9555）出現才建，唔好為咗對稱而開空層。

---

## 10. 新行為 checklist（開檔前逐項打勾）

1. [ ] 寫四行：**輸入 → 輸出、幾時跑、有冇特權、改一次要唔要重燒**。
2. [ ] 用 §4 決策樹定角色；再答「CORE／USER／DATA」。
3. [ ] 定域（英文、短、同現有 `pixel`／`net`／`store` 一致）。
4. [ ] 建對應的家：`components/<role>/<域>_<role>/`、`modules/<role>/<name>_<role>/`、或 `/data/...`。
5. [ ] `REQUIRES` **只列** §5 允許的依賴；USER 只可以 include 契約 header。
6. [ ] 符號用 §6 的命名；公開 header 只放契約。
7. [ ] 效果：確認「畫波寫唔寫得出」——寫得出就 DATA，唔好寫 C／wasm。
8. [ ] 測試：純函數 → `tools/hosttest/`；硬件 → 板測（登記去 [test-register.md](../00_pm/test-register.md)）。
9. [ ] 文件：更新 §7 差距表或 §8 決定；新概念更新 [docs/README.md](../README.md)。
10. [ ] 問一次：**呢個決定係角色、平面、定係內容？** 三者唔好混。

---

## 11. 反模式（見到就退回）

| 反模式 | 為什麼錯 | 正確 |
|---|---|---|
| 效果 `include freertos/`／`driver/` | 效果被綁死 CORE；`.nce` 永遠做唔到 | 時間由 `now_us`；硬件交 host |
| 效果自己 `begin`／`commit` 環 | 效果綁死 single-ring、冇得假消費 | 宿主 task 負責；效果只寫 `dst` |
| 引擎入面有一個具體效果（`breath`） | 工具同實作混埋；換效果要改工具 | 效果放 `effects/`／DATA |
| task 入面有效果數學 | 換效果要改 task；效果冇得獨立測 | 搬去 `effects/`／DATA |
| action 入面有 `nc_px_ring_begin()`／DMA | 政策層唔應該知機制 | action 只設參數／呼叫 effect API |
| driver 入面有 pixel／WS2812 概念 | 換燈種要改驅動；LCD 用不到 | 通用波形介面（`drv_i80` 已經係咁） |
| `sw/` 養行為（狀態機、時間） | `sw` 係演算法庫 | 搬去 `effects/` 或 `tasks/` |
| 一個 component 做環＋編碼＋效果＋宿主 | 四種角色、四種壽命 | 拆四個家 |
| 將 mapping／modes 寫入 C | 每次改配置都要重燒 | DATA（JSON） |
| USER 模組 include 咗 `nc_render.h` | USER 知道 CORE 內部；換 CORE 就爆 | 只 include 契約 header |
| DATA 入面有指標／可執行邏輯 | 壞檔可以執行任意嘢 | 只放值／宣告式 program |
| 所有效果都寫成 C | 重燒成本高、第三方改唔到 | 先用 DATA program，再考慮 `.nce` |

---

## 12. 點樣執行（review ＋ 自動檢查）

**Review 六問**：

1. 這個檔係咪住喺它（角色 × 平面）的家？
2. `REQUIRES` 有冇多過 §5 允許的？
3. CORE 有冇偷偷 include `modules/`？（P1）
4. USER 有冇 include driver／task／環？（P2）
5. DATA 有冇可執行邏輯？（P4）
6. 效果有冇「加一個 JSON／丟一個 `.nce`」就出得到？（要重燒就代表擺錯平面）

**自動檢查**（暫時手動，將來入 CI）：

```bash
# CORE 唔准反向依賴 USER
grep -rn "modules/" firmware/components/ --include='*.c' --include='*.h'

# USER 唔准 include CORE 內部（只准契約 header）
grep -rnE '#include "(driver|sys|tasks|sw|effects)/' modules/ --include='*.c' --include='*.h'

# 引擎（sw/pixel/engine）唔准出現具體效果名
grep -rnE "\b(breath|wave|solid|ramp|zone1)\b" firmware/components/sw/pixel/engine/ 2>/dev/null

# 實作（effects/）只准 include 契約 header，唔准掂引擎內部
grep -rnE '#include "(sw|sys|tasks|driver|bus|host)/' firmware/components/effects/ --include='*.c' 2>/dev/null

# DATA 唔准有 code
find ports/*/*/data -name '*.c' -o -name '*.py' -o -name '*.wasm'
```

五條都應該**零結果**（註解除外）。有結果＝平面被繞過，唔係「例外」。

---

## 13. 一句話總結

> **MP 一個軸：角色。C 兩個軸：角色 × 平面。**
> 效果係一個角色、三種交付（DATA program／CORE C／USER `.nce`）、四層資料（effect／map／mode／playlist）。
> **三個名分開**：引擎（工具）住 `sw/pixel/engine/`、實作住 `effects/<域>_effects/`、
> 呼叫方住 `tasks/`·`action/`·`host/`；設定住 `/data/`。
> 加一個效果應該係「加一個 JSON／丟一個檔」，要重燒 C 才出得到，就代表你擺錯咗平面。
