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
| `0xF06B` 預覽 | >315 px 會 `err=big`（2,600 B 回覆上限） | `show-pipeline.md` §6 |
| white mix | 整幀統一套用；per-mode 動畫／fade 未做 | `show-pipeline.md` §6 |
| NCM 平台 | 只 S3（`idf_component.yml` rules `target == esp32s3`）；release 預設 OFF，只 `--tests` 開 | `firmware/CMakeLists.txt` |
| ESP-NOW | 只係 test 指令（`nc_test_action.c:5417-5419`），**唔係 transport**；`AGENTS.md` §2.2 寫成收幀通道（文件錯） | 見 §5 |
| AOT | `nc_mod_engine.c:374-375` 明寫「AOT 未接，只收 wasm」 | `ncm-contract.md` §5.1 |

---

## 4. 效果層（pixel）專項

- **效果種類**：28（16IO）／40（驗證過）全部係 program 型（sine／square／pulse 組合），
  **冇 image／video 類**；hex／text 走 textkit project。
- **三層分家做得清**：引擎 `sw/pixel/engine/`、實作 `effects/pixel_effects/`（6 個 C 效果）、
  資產 `/data/pixel/`；`.nce` 係第四種（USER 平面），但合成未做。
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
| `AGENTS.md` §2.2 誇大 transport | 寫「USB／UART／TCP／UDP／WS／ESP-NOW／I2C／SPI／CAN 收幀」；實際 `nc_link_kind_t` 只有 CDC／USBJTAG／UART／TCP／UDP／WS／HTTP | 建議下次改 AGENTS 時修正 |
| `roadmap.md` 過時 | 仍寫「Phase 2 pixel pipeline 未開始」 | 實際已 production |
| `next-steps.md` 過時 | 2026-11-04 版，多數已完成 |  |
| `hw-coverage.md` | 4 個 unit 上限寫錯（I80 2→1、I2S 1→3、UART 3→6、I2C 2→3）；缺 9 類資源 ≈ +185 key；S3 寫 `ETH driver=rmii` 係真 bug 級風險 | 該文自己已列 |

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

### 6.3 但唔可以原封搬入 `driver/`（三條硬約束）

| # | 約束 | 證據 |
|---|---|---|
| 1 | `component-layout.md` §1 規則 1：**driver 只准 include `esp_driver_*`／`soc`** | `nc_mod.c` 用 `task_manager`／`config`／`mem`／`ota_action` → 直接違反 |
| 2 | 開機次序：ncm 要喺 B06 declare／B07 commit／B08 start **之後**（`main.c:4553`） | 本質係 sys 層生命週期，唔係 leaf driver |
| 3 | 引擎自己都未係 leaf | `nc_mod_engine.c:31` include `nc_ota_action.h`；又用 `nc_mod_name()`／`nc_mod_payload()`／`nc_mod_decl()`（`nc_mod.h:75-80`）→ 要落 driver 先要解耦 |

> 即係：「唔獨立存在」✅；「住 `components/driver/`」❌（原封搬一定破規則）。
> 要用家講法成立，只有下面兩條路。

### 6.4 兩個合規落位

**A（最小改動，建議先做）：`components/sys/ncm/` 整包搬**

- `host` 角色消失；引擎繼續用 vtable（將來換 AOT／其他 runtime 只換 `nc_mod_engine.c`）。
- 技術可行：`components/sys` 係 container（冇 CMakeLists），`sys/nc4`／`sys/mem` 已經係咁；
  `components/sys/ncm` 同樣唔會被 IDF 自動登記，`NC_ENABLE_NCM` gate 照舊。
- sys → 自己子組件，冇新增依賴方向。
- 代價：`sys` 多一個 optional ＋ WAMR 依賴。**文檔要補一句**「`sys/` 之下可以有
  optional 子組件（目前唯一例外：ncm）」，否則下一個 AI 會以為 sys 全部開機必需。
- 剩餘工作：把 `ota_action` 依賴剪走（改成 sys 層的 quiet flag／hook）—— 同 §5 第二行同一件事。

**B（最貼近「沙箱驅動」講法，較大）：拆兩件**

```text
driver/drv_wamr/    只做 runtime 綁定：init／instantiate／call／imports（＋idf_component.yml）
                    interface 唔識 nc_mod_t（傳 name／payload／decl＋callbacks）
                    ota pause 搬走 → 呢層零 sys 依賴
sys/ncm/            scan／容器解析／arena／live／看門狗／task／status
```

- `sys → driver` 係合法方向（`component-layout.md` §2），所以 B 守得規矩。
- 代價：改 `nc_mod_backend_t` 介面＋`nc_mod.c` 呼叫點＋CMake／host test，估 1–2 日。
- 好處：`driver/` 真係有一個可換的「沙箱驅動」；WAMR 完全隔離（連 header 都唔會漏上層）。

**建議**：先 A（今日可完成、風險低，已經達成「唔獨立存在」）；B 留到
（i）真要第二個 backend，或（ii）要上 P4 AOT 時才做 —— 因為 B 的價值要等第二個實作才出現。

### 6.5 遷移 checklist（未做）

- [ ] `git mv firmware/components/host/ncm firmware/components/sys/ncm`
- [ ] `firmware/CMakeLists.txt`：`NC_ENABLE_NCM` 段的 path（註解亦要改）
- [ ] 全樹 `grep -rn "components/host"`：AGENTS §3、`component-layout.md` §1／§3、
      `behavior-taxonomy.md` §2.4、`ncm-contract.md`、`ncm-loader.md`、`module-adoption.md`
- [ ] `NC_ENABLE_NCM=ON`（tests）同 OFF（release）兩種 build 都過
- [ ] `bash tools/hosttest/run_all.sh`
- [ ] 真機：`status tasks` 見到 ncm 任務、載入一個 `.nct` 確認 tick 正常
- [ ] `sys` optional 子組件規則寫入 `component-layout.md` §1

---

## 7. 建議次序

| 序 | 項目 | 理由 | 前置 |
|---|---|---|---|
| 1 | **RMT 後端** | 唯一「有板都用唔到」的硬缺口；C3 係完全受害者 | 要 C3 板／邏輯分析儀 |
| 2 | **0x3400 dispatcher** | 細，令外掛功能閉環 | 無 |
| 3 | **host → sys/ncm 搬位**（§6.4 A）＋剪 `ota_action` 依賴 | 架構債，改動細 | 無 |
| 4 | **OTA rollback** | 風險，唔係功能 | partition 已夠（1.5 MB × 2） |
| 5 | **PARLIO** | 要 P4 板 | P4 板 |
| 6 | **跨機同步** | 產品級，最大 | 要兩塊同款板 |

---

## 8. 來源（今次睇過的檔）

- `firmware/CMakeLists.txt`、`firmware/main/main.c`、`firmware/main/nc_wifi.c`
- `firmware/components/host/ncm/*`、`firmware/components/tasks/render_task/*`、
  `firmware/components/sys/config/nc_cfg_load.c`、`firmware/components/driver/*`
- `docs/03_notes/component-layout.md`、`behavior-taxonomy.md`、`pixel-output-backends.md`、
  `hw-coverage.md`、`ncm-contract.md`、`show-pipeline.md`、`effect-language.md`
- `docs/00_pm/acceptance-2026-11-28.md`、`status.md`（§7.31x–§7.338）、
  `pixel-tiers-plan.md`（Task 9）、`roadmap.md`、`next-steps.md`、
  `goal-hardware-acceleration.md`
