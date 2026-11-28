# 固件缺口清單（gap review）

> **日期**：2026-11-28
> **範圍**：`firmware/`（CORE 平面），唔包 web_tools／tools。
> **方法**：逐個 component 讀 code ＋ 對 `status.md` §7.33x／`acceptance-2026-11-28.md`／
> `pixel-output-backends.md`／`hw-coverage.md`。**今次冇上板**：sandbox 開唔到 serial
> （`could not open port … Operation not permitted`），所以下面全部係 code／文件證據，
> 唔係即場 `status` 的真機數字。凡標「未驗」就係未驗，唔可以當已驗。
> **用途**：回答「基礎功能仲缺咩」。結論係「核心路徑冇已知缺陷，但功能面有幾條路未鋪」。

---

## 0. 一句結論

**冇寫壞，係未寫。** 已上板驗證的 S3 ＋ I80 直驅那條路（NC4／config／state／pixel 四層／
OTA）確實冇基礎功能缺陷；但「支援的晶片都用得」未成立 —— 10 條硬缺口，其中 3 條係
**有板都用唔到**（RMT／PARLIO／真面板）。

另外兩個「唔應該存在」候選：`components/effects/` 5 個 C 效果（**零呼叫，死代碼**，§4.1）
同 production project 仍然開住的 `0xF062` PXBENCH（§5.5）。

---

## 1. 兩層判斷

| 層 | 判斷 | 依據 |
|---|---|---|
| 核心路徑（S3＋I80＋NC4＋config/state＋OTA） | ✅ 同意冇基礎缺陷 | `acceptance-2026-11-28.md` §2／§3：18/18 build、host 全綠、真機 50 fps／cache 4/4／NCSTATE hit。**呢個係報告寫的，今次冇重跑** |
| 「每塊支援的晶片都用得」 | ❌ 未成立 | 見 §2 #1／#2：C3 冇任何後端出得到 WS2812；P4 出燈要佔 LCD_CAM |

---

## 2. 硬缺口（功能未鋪）

嚴重度：**高**＝用家會直接撞到；**中**＝功能少一半；**低**＝暫時有替代。

| # | 缺口 | 現況證據 | 影響 | 嚴重 |
|---|---|---|---|---|
| 1 | **RMT 後端** | `render_task/` 只有 `i80`／`spi`／`direct`／`mock`；`driver/` 冇 `drv_rmt`；`nc_cfg_types.h:271`「schema 已收；後端未實作」；`nc_render_task.c:592` 會大聲報錯停 lane | **C3 完全點唔到 WS2812**：C3 冇 LCD_CAM（`hw-coverage.md` §2）＋冇 RMT → 只剩 SPI 時脈式（WS2801／APA102） | 高 |
| 2 | **PARLIO 後端** | `nc_cfg_types.h:272`；`pixel-tiers-plan.md` Task 9 Step 3–4（未做） | P4 想出燈就要佔 LCD_CAM → 燈同真面板搶同一個周邊 | 中 |
| 3 | **真 LCD 面板** | `nc_i80_drv_direct.c:363` 自己寫「未上板」；esp_lcd 版要齊 data＋WR＋DC；多 device／CS 仲裁未做 | 設計紅線話要保留面板能力，但**一條都未證** | 中 |
| 4 | **ETH SPI PHY ＋ static IP** | `main.c:3983`「SPI PHY 未實作」；`main.c:4040`「static 未實作」 | S3 冇內建 MAC → **S3 有線網絡＝零**，且只有 DHCP | 中 |
| 5 | **NC4 `out` 轉發** | `main.c:3499`：「今日只實作 `in → decodeN`；`out` 裡 link 轉發同 `outbound` 未實作」 | 冇 gateway／bus：一台帶唔到幾台下線 | 中 |
| 6 | **外掛私有指令 0x3400–0x3FFF** | manifest 有 `cmds[]`，`nc_mod_cmd_at()`（`nc_mod.c:1000`）**零呼叫**；`commands.md:45` 亦寫「路由，未實作」 | action 模組寫咗都收唔到指令 → 外掛生態只通一半 | 中 |
| 7 | **device 層只有燈** | `nc_cfg_load.c:1060` 收 I2C／I2S／ENC／XL9555 但只 `apply_eth/sdio/pin/pwm/uart/i80/spi`；`nc_cfg_load.c:1655` 只讀 `LED` | TFT／PCA9685／XL9555／I2C 裝置全部用唔到（enable=1 只 warn） | 中 |
| 8 | **OTA rollback** | `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` 冇開；`nc_ota_action.h:6` 2212／2214／2216 未實作 | 推壞 image 要 BOOT 救（AGENTS §8.1 自己寫「Rollback 未開」） | 中 |
| 9 | **跨機同步** | `roadmap.md` Phase 3 未開始；`goal-hardware-acceleration.md` M4–M5 未動；firmware 冇 timebase／scheduled start 實作 | 多塊板拼一個大畫面：固件層面係零 | 高（產品級） |
| 10 | **UI／BLE** | `components/ui/` 唔存在；`bt.pin` 冇人讀（`main.c:449`） | 機身頁面／藍牙都冇 | 低 |

---

## 3. 半成品／未驗證（有骨架，未閉環）

| 項目 | 現況 | 邊個文件講 |
|---|---|---|
| 16 lane 視覺 | build＋計時都過，但**未有 16 條實體燈鏈**，視覺未驗 | status §7.312 |
| `.nce` 合成 | program interpreter 只認 6 種段型別（`nc_px_asset.h:32-38`）；`.nce` 資產唔會合成入 show | `show-pipeline.md` §6 |
| **內建 C 效果係死代碼** | `components/effects/pixel_effects/` 5 個 `nc_effect_*`（~166 行）＋ `nc_effect_table.c`；`nc_pixel_effects_register()` **零呼叫**（只有 header 註解同 `pxEngine_test`），`nc_px_engine_frame()` 同理只 test 用 | 見 §4.1；仍然每塊板都編 |
| `0xF06B` 預覽 | >315 px 會 `err=big`（2,600 B 回覆上限） | `show-pipeline.md` §6 |
| white mix | 整幀統一套用；per-mode 動畫／fade 未做 | `show-pipeline.md` §6 |
| NCM 平台 | 只 S3（`idf_component.yml` rules `target == esp32s3`）；release 預設 OFF，只 `--tests` 開 | `firmware/CMakeLists.txt` |
| ESP-NOW | 只係 test 指令（`nc_test_action.c:5417-5419`），**唔係 transport**；`AGENTS.md` §2.2 寫成收幀通道（文件錯） | 見 §5 |
| AOT | `nc_mod_engine.c:374-375` 明寫「AOT 未接，只收 wasm」 | `ncm-contract.md` §5.1 |

---

## 4. 效果層（pixel）專項

### 4.1 「效果」呢個字實際係四樣材料

| 名 | 位置 | 係咩 | production 行唔行 |
|---|---|---|---|
| **引擎** | `sw/pixel/engine/`（`core` 717＋`program` 272＋`tables` 81 行） | program interpreter（6 種段型別）、sin／gamma 表、registry | ✅ `program_value()`／`tables_init()`／`gamma` 由 `pixel_effect_task` 叫 |
| **內建效果實作** | `components/effects/pixel_effects/`（5 個 `nc_effect_*`，合共 ~166 行） | 填 registry 的實作 | ❌ **零呼叫**（見 §3 最後一行） |
| **資產** | `/data/pixel/effects/*.json` | program（段型別＋顏色／區段） | ✅ 真正行呢條 |
| **`.nce`** | `/data/**/effects/*.nce` | WASM 效果（USER 平面） | ❌ 未合成 |

> 所以「引擎由 `effects/` 操作」係倒轉：`effects/` 只係實作，交去引擎 registry；
> 而 registry 條路（舊 8×8 引擎）2026-11-25 退役時，登記嗰句一齊冇咗 →
> 5 個 C 效果變成**死代碼**，但仍然每塊板都編。

### 4.2 模式（mode）住邊

「模式管理」**冇獨立 component**，全部住喺 `tasks/pixel_paint_task/`（2,117 行）：

| 動作 | 位置 |
|---|---|
| 掃 `modes/*.json`、解析成 `nc_px_mode_tbl_t` | `sw/pixel/asset/nc_px_asset_scan.c`、`nc_px_asset.c` |
| 現在播邊個 mode | paint task `s_content.mode_name_id` |
| 手動切 mode（`0xF06D`） | paint task `s_pending_mode` → 幀邊界 rebuild |
| 自動輪播（registry `list[]`） | paint task `mode_frames`／`mode_limit`（`maxF`） |
| reload（`0xF06F`） | paint task |
| plan／6 B layout／`.ncc` cache | paint task |

> 呢個係結構問題：paint task 同時做「播咩」（policy）同「點上色」（mechanism）。

### 4.3 一幀的真實流程

```text
/data/pixel/{registry.json,modes/,map/,effects/}
   │ paint task（reload／切 mode 才做）：掃描 → 建 plan（groups→entries→asset 引用）
   │                                   → 6 B layout（runs）＋ .ncc cache
   ├─ nc_pipe_aprog_publish(aprog) ──────────────► effect task
   │                                                 每 px：nc_px_engine_program_value()
   │                                                 → 12-bit 值流（雙 pool＋ack）
   ◄──────────── nc_pipe_effect_copy（只傳指標，唔複製值流）
   │ paint task：nc_px_layout_apply_dither() 寫環（4 B/px，R,G,B,W）
   │             → white mix → nc_px_ring_commit()
   ▼ 環
   encode task：環格 → 該後端 bus word／bitstream（i80／spi）
   ▼
   render task：DMA 完成 → submit → 燈
```

### 4.4 量測／缺口

- **效果種類**：28（16IO）／40（驗證過）全部係 program 型（sine／square／pulse 組合），
  **冇 image／video 類**；hex／text 走 textkit project。
- **量測齊**：frame 19.99 ms／50 fps、pair 3.12、apply 2.35、encode 9.39、PSRAM 1.82 MB free；
  16×600 極限 CPU ~17.6 ms vs 20 ms 預算（margin 2–5 ms，偏緊）。
- **真缺口**：RMT／PARLIO（同 §2 #1／#2）；16 lane 視覺；`.nce` 合成。

---

## 5. 工程債／文件漂移

| 項目 | 證據 | 備註 |
|---|---|---|
| `main.c` 仍係裝配＋handler 大雜燴 | 5,091 行、24 個 `on_*` handler | `component-layout.md` §10 自己問「main.c 有冇 if/for/字串解析」 |
| host → action 反向依賴 | `nc_mod_engine.c:31` include `nc_ota_action.h`、`:456` 讀 pause | 全樹唯一一條這種依賴；睇 §6.3 |
| 冇 loop 停滯偵測 | `next-steps.md` §1：`monitor` 2026-11-04 已刪、`epoch` 已移除 | task WDT 仍在，但卡 loop 唔會報 |
| **`NC_ENABLE_NCM` 開關係 inert** | `main.c` 所有 ncm 段落都在 `#ifdef NC_ENABLE_TESTS`（`:4476`／`:4482`／`:4527`／`:4537`／`:4567`）；單獨開 `NC_ENABLE_NCM` 只會編 component、冇人用 | release 其實完全冇 ncm |
| **AGENTS §5 把 NCM 列做核心（房東）** | 原文「核心：nc4、net、core、NCM 引擎」；實際 code 係 test-only | 文件同 code 相反，係「位置錯」觀感的來源 |
| **模式管理住喺 paint task** | `nc_px_paint_task.c` 2,117 行同時有 mode 選擇／輪播／plan／cache／`0xF06x` | policy 同 mechanism 混在一齊；睇 §4.2 |
| **`0xF062` PXBENCH 喺生產 project 開住** | `NETCORE_PXBENCH` 預設 n，但 6 個 project 明開（含 `Matrix-LED-Textkit`、`Zero-Show*`） | 見 §5.5 |
| `0xF062` 有兩份實作 | `main.c:1313`（`CONFIG_NETCORE_PXBENCH`）＋`nc_test_action.c:4125`（`NC_ENABLE_TESTS`）；兩者同時開 → `nc_action.c:115` 丟後來者並 `ESP_LOGE` | 只影響 tests build 的 log，唔影響行為 |
| `AGENTS.md` §2.2 誇大 transport | 寫「USB／UART／TCP／UDP／WS／ESP-NOW／I2C／SPI／CAN 收幀」；實際 `nc_link_kind_t` 只有 CDC／USBJTAG／UART／TCP／UDP／WS／HTTP | 建議下次改 AGENTS 時修正 |
| `roadmap.md` 過時 | 仍寫「Phase 2 pixel pipeline 未開始」 | 實際已 production |
| `next-steps.md` 過時 | 2026-11-04 版，多數已完成 |  |
| `hw-coverage.md` | 4 個 unit 上限寫錯（I80 2→1、I2S 1→3、UART 3→6、I2C 2→3）；缺 9 類資源 ≈ +185 key；S3 寫 `ETH driver=rmii` 係真 bug 級風險 | 該文自己已列 |

### 5.5 bench：邊個真係入 firmware

| bench | 位置 | 幾時入 image | 建議 |
|---|---|---|---|
| `0xF062` PXBENCH | `sw/pixel/nc_pixel_bench.c`（178 行）＋`main.c:1313` | `CONFIG_NETCORE_PXBENCH`（預設 n）；**6 個 project 明開**，含 `Matrix-LED-Textkit`（production） | 唯一「量產 image 都有 bench」的；量產 project 應該關，或搬入 `NC_ENABLE_TESTS` |
| `0xF065/67/69/77` | `components/bench/effect_bench/`（4 檔，3,190 行） | 只 `EffectPerf` project（`NC_PROJECT MATCHES EffectPerf`）→ release 一定冇 | 係唯一真機量測手段；刪 = 以後冇回歸數字。要清就整包刪 ＋ 停用 `effect_bench.py`／`layout_bench.py` |
| `gamma/sin/program/pxMap` | `tools/hosttest/*_bench/` | 唔入 firmware | 唔關 firmware 事；4 支唔在 `run_all` |

> 判準（`AGENTS.md` 鐵律 11）：`firmware/` 只放「需要跟 firmware 一起編」的量測。
> 所以 bench 唔可以搬去 `modules/lab/`（lab 係主機側 orchestration）；
> 要「唔存在」就只有兩個做法：**Kconfig 永久關** 或 **整段刪**。

---

## 6. `/host/` 的定位

### 6.1 現況（事實）

| 項 | 值 |
|---|---|
| 位置 | `firmware/components/host/ncm/`（`nc_mod.c` 1,199 行、`nc_mod_engine.c` 536 行、其餘 4 檔；合計 2,683 行 C／5 個 header） |
| 做咩 | 掃 `/data/**/*.nct|.nce` → 解容器 → 由 `ncm.arena` 切 linear memory → WAMR 執行 → 效果 `nce_render()`、任務 `task_tick()`（白名單 import：`nc_gpio_*`／`nc_print*`／`nc_now_us`）→ 掛入 `task_manager` |
| 對外 API | `nc_mod_init/scan/load/select/tick/status_json`；backend 用 vtable `nc_mod_backend_t`（5 hooks） |
| 有幾個 backend | **1 個**：`nc_mod_wamr_backend()`（`nc_mod_engine.c:524`）。用家講「只有一個沙盒驅動」——確認正確 |
| 誰用 | 只有 `main.c`（`main.c:158`、`:4553`） |
| Build gate | `NC_ENABLE_NCM` 預設 **OFF**；`--tests` 自動 ON（`firmware/CMakeLists.txt`） |
| 平台 | `idf_component.yml` rules 只准 `target == esp32s3`（WAMR Xtensa）；P4 要等 AOT |
| 依賴 | `mem`／`esp_rom`／`freertos`／`esp_timer`／`task_manager`／`config`／**`ota_action`**／WAMR／`esp_driver_gpio` |

### 6.2 用家判斷（2026-11-28）

> 「`/host/` 應該是屬於驅動類，而且只有一個沙盒驅動。我因此認為他不應該自己獨立存在。」

**方向記錄：`host` 唔應該做一個獨立角色／獨立頂層目錄。** 呢個判斷同意：
`host` 係七個 CORE 角色入面唯一一個「得一個實作、又只係一個 backend」的角色，
為佢開一個位，等於用目錄表達「有一個 runtime」——但 runtime 本質係可換的實作。

### 6.3 但唔可以原封搬入 `driver/`（四條硬約束）

| # | 約束 | 證據 |
|---|---|---|
| 1 | `component-layout.md` §1 規則 1：**driver 只准 include `esp_driver_*`／`soc`** | `nc_mod.c` 用 `task_manager`／`config`／`mem`／`ota_action` → 直接違反 |
| 2 | 開機次序：ncm 要喺 B06 declare／B07 commit／B08 start **之後**（`main.c:4553`） | 本質係 sys 層生命週期，唔係 leaf driver |
| 3 | 引擎自己都未係 leaf | `nc_mod_engine.c:31` include `nc_ota_action.h`；又用 `nc_mod_name()`／`nc_mod_payload()`／`nc_mod_decl()`（`nc_mod.h:75-80`）→ 要落 driver 先要解耦 |
| 4 | 「格式工具」直覺撞依賴方向 | `nc_ncd.c`（容器解析 164 行）係純 function，最想放 `sw/`；但 `sw → sys`，loader 喺 `sys` 就唔可以 include `sw` → parser 只可以**同 loader 一齊住 `sys`**，或者**沉到 `driver/`** |

> 即係：「唔獨立存在」✅；「住 `components/driver/`」❌（原封搬一定破規則）。
> 要用家講法成立，只有下面三條路。

### 6.4 用家方向 v3（2026-11-28）：一套「插件工具」＝解碼＋運行

> 「解碼器……其實並不單止解碼功能，因為解碼完後他們都要嘗試去運作插件；
> 所以真實需要的工具，是應該有一個工具來運作插件。」

**即係「獨立的一套工具」唔止 decode，而係 decode ＋ 實例化 ＋ 跑 ＋ 收工。**
`.nct`／`.nce` 係同一件事的兩種 payload（分別只在 manifest `kind` 同 entry 名：
`task_tick()` vs `nce_render()`），所以係**一個工具**，唔係兩個。

| 工具內部 | 檔 | 識咩 |
|---|---|---|
| 解碼 | `nc_ncd.c`＋`nc_meta.c` | bytes → 容器／manifest／meta |
| 實例 | 新 `nc_plugin_open/close` | 由 caller 給的 arena 切 linear memory、instantiate |
| 執行 | 新 `nc_plugin_tick/render` | 叫 entry、拿回傳值／framebuffer |
| backend | `nc_mod_engine.c`（WAMR） | **唯一**識 WAMR 的地方（已有 vtable，可換 AOT） |

**對外只係四個函式**：`open(payload, size, decl)`／`tick(h)`／`render(h, fid, t)`／`close(h)`。
工具**唔識** pixel、**唔識** `nc_task_t`、**唔識** status、**唔管**幾時跑、為邊個跑。

#### 6.4.1 客戶端（工具唔管政策）

| 客戶端 | 用途 | 落位 | 今日 |
|---|---|---|---|
| pixel | `.nce` → canonical frame | `sw/pixel/`（adapter 未寫） | ❌ 未接 |
| task | `.nct` → `nc_task_t` 的 tick | `tasks/` | 只有 test 用 |
| 測試 | 掃目錄、arena 帳、`0xF0xx`、status | `test_ncm/`（自註冊） | main 的 6 段 `#ifdef` |

`nc_mod.c`（1,199 行）今日把上面三樣混在一齊：scan／arena／live／預算（policy）＋
attach 任務（task）＋status（sys）。搬嘅時候係**拆佢**，唔係搬佢。

#### 6.4.2 目標樹（暫名）

```text
sw/plugin/           解碼＋實例＋執行（唯一識 WAMR，vtable 包住）
sw/pixel/...         .nce → canonical frame 的 adapter（未寫）
tasks/...            .nct → nc_task_t 的 wrapper
.../test_ncm/        scan／arena／0xF0xx／status（NC_TASK_AUTO_REG／NC_ACTION_TABLE_DEF）
```

- 依賴方向全部合法：`sw → sys`；`effects`／`tasks`／`action` → `sw`。
- `sys` 唔需要呢個工具（arena 由 caller 給）→ §6.3 約束 4 自動消失。
- `CONFIG_NETCORE_PIXEL=n` 的板仍然可以有 `.nct`（工具唔綁 pixel）。
- 同 v2 的分別：v2 把「解碼器獨立、其餘入 pixel」；v3 係「解碼＋運行成套工具獨立，
  pixel 只係其中一個客戶端」。

**阻礙（2026-11-28 核對；✅ = 已清）**

- ✅ 引擎唔再叫 `nc_mod_name/payload/decl`：改收 `nc_plugin_src_t`（name／payload／size／fmt／kind）。
- ✅ 引擎唔再 include `nc_ota_action.h`：暫停閘改由呼叫方注入（`nc_plugin_set_pause_check`）。
- ⬜ `status_add` 係 `main.c:1380` 的 static → 要加 `nc_status_register()`（未做）。
- ⬜ `nc_res.c` 改 `tasks.json` 語義 → 應該由測試 glue／task 側做，唔入工具（未做）。

### 6.5 遷移 checklist（未做）

- [x] 開 `sw/plugin/`：`git mv` `nc_ncd.c`／`nc_meta.c`／`nc_mod_engine.c`（engine → `nc_plugin_wamr.c`）
- [x] 定義 `nc_plugin_set_arena/set_backend/open/step/close/deinit`；engine 經 opaque handle
- [x] 剪 `nc_ota_action.h`（暫停閘由呼叫方注入）
- [ ] `nc_mod.c` 三拆：scan／arena → `test_ncm`；task attach → `tasks/`；status → sys
- [ ] `.nce` adapter 入 `sw/pixel`（可以之後才做，唔阻塞搬位）
- [ ] `test_ncm` 自註冊任務＋指令；刪 main 的 6 段 `#ifdef`
- [ ] 取消 `NC_ENABLE_NCM`，只留 `NC_ENABLE_TESTS`
- [ ] 文件：AGENTS §3／§5（NCM 唔再係核心）、`component-layout.md` §1／§3 刪 `host` 角色、
      `behavior-taxonomy.md` §2.4 刪 host 列、`ncm-contract.md`／`ncm-loader.md` 的 path
- [ ] `NC_ENABLE_TESTS=ON`／`OFF` 兩種 build ＋ `run_all.sh`

---

## 7. 建議次序

| 序 | 項目 | 理由 | 前置 |
|---|---|---|---|
| 1 | **RMT 後端** | 唯一「有板都用唔到」的硬缺口；C3 係完全受害者 | 要 C3 板／邏輯分析儀 |
| 2 | **0x3400 dispatcher** | 細，令外掛功能閉環 | 無 |
| 3 | **解碼器／宿主分家**（§6.4）：開 `sw/ncd`＋`sw/pixel/mod`＋`test_ncm`，取消 `NC_ENABLE_NCM` | 令 tree 同 code 一致（今日 ncm 已經係 test-only，但文件講佢係核心） | 要先加 `nc_status_register()`；剪 `ota_action` 依賴 |
| 5 | **量產 project 關 `PXBENCH`**（§5.5）＋清 `effects/` 死代碼 | 一行 sdkconfig／一個目錄；減少 image 同誤解 | 無 |
| 4 | **OTA rollback** | 風險，唔係功能 | partition 已夠（1.5 MB × 2） |
| 6 | **PARLIO** | 要 P4 板 | P4 板 |
| 7 | **跨機同步** | 產品級，最大 | 要兩塊同款板 |

---

## 8. 來源（今次睇過的檔）

- `firmware/CMakeLists.txt`、`firmware/main/main.c`、`firmware/main/nc_wifi.c`、
  `firmware/main/Kconfig.projbuild`
- `firmware/components/host/ncm/*`、`firmware/components/tasks/render_task/*`、
  `firmware/components/sys/config/nc_cfg_load.c`、`firmware/components/driver/*`
- 效果／模式：`firmware/components/sw/pixel/{engine,asset}/*`、
  `firmware/components/effects/pixel_effects/*`、
  `firmware/components/tasks/pixel_{paint,effect}_task/*`、
  `firmware/components/sw/pixel/nc_pixel_bench.c`、`firmware/components/bench/effect_bench/*`
- `docs/03_notes/component-layout.md`、`behavior-taxonomy.md`、`pixel-output-backends.md`、
  `hw-coverage.md`、`ncm-contract.md`、`show-pipeline.md`、`effect-language.md`
- `docs/00_pm/acceptance-2026-11-28.md`、`status.md`（§7.31x–§7.338）、
  `pixel-tiers-plan.md`（Task 9）、`roadmap.md`、`next-steps.md`、
  `goal-hardware-acceleration.md`
