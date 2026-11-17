# 測試冊（test register）

> **用途**：追蹤每一個功能／測試而家喺邊個「收納層級」、用咩證據驗證、性能數字係
> 幾多、下一步係咩。收納入正式固件之前，呢度一定要有一行、並且引用 test id。
>
> 規則：**只追加、不覆蓋**（狀態可以更新，但歷史數字留住）。ID 一旦發出唔改。
> 層級定義見 [module-adoption.md](../03_notes/module-adoption.md)：
> **L0** 外掛 wasm 直譯 → **L1** 外掛 AOT → **L2** 原生 C 固件 → **L3** 硬件加速。

## 1. 目前條目

| ID | 測試／項目 | 類型 | 層級 | 位置 | 驗證證據（可重跑） | 性能／資源 | 狀態 |
|---|---|---|---|---|---|---|---|
| T-NCM-001 | 外掛任務載入鏈：掃描 → 建表 → attach → WAMR instantiate → tick | 加載 | L0 | `nc_mod.c`／`nc_mod_engine.c` | `status ncm`：`backend:"wamr"`、模組 `ok:1`、`attached:1`、`frames` 增長；`status tasks` 見租戶 | 每模組 linear memory 64 KB（PSRAM） | ✅ 2026-11-17 驗證 |
| T-NCM-002 | `hello_task`：列印＋讀 GPIO21＋反轉 GPIO21（1 Hz） | 功能 | L0 | [tools/ncm_samples/hello_task.wat](../../tools/ncm_samples/hello_task.wat) | CDC log 每秒 `[hello_task] tick pin21 read=1 toggle=0`／`read=0 toggle=1` 交替 | 每 tick ≪ budget 20 ms | ✅ 驗證（保留作 L0 golden） |
| T-NCM-003 | wasm 直譯 vs 原生 C 同 workload（50,000 LCG） | 性能 | L0／L2 對照 | [bench_task.wat](../../tools/ncm_samples/bench_task.wat)＋`0xF050 CPU_BURN` | checksum 一致 `1919499729`；C 1,683 µs、wasm 24,887 µs → **14.8×** | flash +92 KB（WAMR；僅 tests 建置）；內部 heap ~15 KB | ✅ 2026-11-17 量測 |
| T-NCM-004 | 外掛任務運行中做 NC4 OTA（OTA pause） | 穩定性 | L0 | `nc_ota_action_paused()` | BEGIN 4.9 s、WRITE 6.1 s（209 KB/s）、END ok、APPLY 後 `rst=3`；無 TASK_WDT | — | ✅ 驗證 |
| T-NCM-005 | `resources.json` seed＋project preset | 配置 | L0 | `main.c seed_presets()` | `cfg.res.miss=0`；刪檔再開機重 seed 出 519 B preset | — | ✅ 驗證 |
| T-NCM-006 | WAMR × IDF 6 啟動安全（qsort const／pthread_self／memmap） | 移植 | L0 | [tools/patch_wamr.py](../../tools/patch_wamr.py)＋`firmware/CMakeLists.txt` | 冷開機 B01–B11 全 ok、`backend:"wamr"`、無 panic；`patch_wamr.py --check` = 0 | 補丁 4 個位 | ✅ 驗證 |
| T-NCM-007 | `NC_PLAN_CLS_BULK` 實際 class（reply 幀在 PSRAM） | 記憶體 | L2 | `nc_plan.c`／`main.c` | `status mem.plan`：`ok:true`、`nc4.reply0/1 cls:"psram"`；release 內部 heap 174,015 B | — | ✅ 驗證 |
| T-NCM-008 | 惡意／壞 `.nct` 容器防護（fuzz：截斷、超長 count、錯 meta size） | 安全 | L1-待 | loader + PC fuzz | **未做** | — | ⬜ 下一步 |
| T-NCM-009 | AOT 外掛（`fmt=aot`，S3 xtensa） | 性能 | L1 | 未接（`CONFIG_WAMR_ENABLE_AOT=n`） | **未做**（wamrc、PSRAM dual-bus 執行路徑） | 預期 2–4× 慢 vs C（待測） | ⬜ P3 |
| T-NCM-010 | LED pattern 任務外掛化（`nc_led_show/info`，driver 留 host） | 功能 | L0→L2 | 未做（先加 `nc_cap_led`） | **未做** | 邊界 copy 2.7 KB ≈68 µs（既有數據） | ⬜ 下一步 |
| T-NCM-011 | 每模組獨立 loop／prio／core／period（真任務調度） | 架構 | L0→L2 | loader 已 attach、未調度 | **未做** | — | ⬜ 下一步 |
| T-NCM-012 | TCP 吞吐外掛化 | 功能 | L2-保留 | 安全政策未開 network 能力 | 唔搬：測 host lwIP | — | ⏸ 決定不開放 |
| T-NCM-013 | Native ELF 外掛（`espressif/elf_loader`，trusted only） | 可選 L1.5 | 未做 | 官方 component；S3 可在 PSRAM 執行 ELF | **未做** | ≈1× C（PSRAM 取指待量）；**無沙箱** | ⬜ 可選 PoC（簽名＋ABI 白名單齊備先做） |
| T-NCM-014 | L3：S3 RMT LED matrix（DMA→RMT） | 硬件加速 | L3 | 未做（現有 `test_rgb_led_matrix` 為 CPU 波形） | **未做** | 目標 60 fps＋、CPU <10%、零 bit-bang | ⬜ M4 第一條 pipeline |
| T-NCM-015 | L3：P4 CAM→ISP→PPA→JPEG pipeline | 硬件加速 | L3 | 未做（`esp_driver_cam/isp/ppa/jpeg`） | **未做** | 目標 640×480@30 fps、CPU 只做控制 | ⬜ M4（要接線） |
| T-NCM-016 | L3：LCD/顯示（PPA/DMA2D→LCD_CAM） | 硬件加速 | L3 | 未做 | **未做** | 全屏刷新 CPU ~0、無 tearing | ⬜ M4/M5 |
| T-NCM-017 | API 呼叫成本（`nc_now_us`／`nc_gpio_read`） | 性能 | L0 | [api_cost.wat](../../tools/ncm_samples/api_cost.wat) | 空迴圈 725 µs／2000；now 4805 µs；gpio 5440 µs；LCG 993 µs（N=2000，多次中位數） | **約 2.0–2.4 µs／call**（其餘係迴圈本身 0.36 µs/iter） | ✅ 2026-11-17 量測 |
| T-NCM-018 | 任務模組獨立編譯工具 | 工具 | — | [tools/ncm_build.py](../../tools/ncm_build.py)＋`<name>.build.json` | `hello_task`／`bench_task`／`api_cost` 三個模組由工具建出；輸出 887／872／519 B | — | ✅ 2026-11-17 驗證 |

## 2. Host 測試（完全唔入固件，L2 工具）

`tools/hosttest/run_all.sh`：**12/12 全過**（2026-11-17）：
`cfg`、`cfgstruct`（含 resources 層）、`ncd`、`task`、`router`、`link`、`mod`、
`hub`、`plan`、`buf`、`portgen`、`ncm`。

## 3. 固件內測試／工具任務處置（2026-11-17 重組後）

規則：**功能測試用外掛模組（動態加載）；效能測試留 dev 固件**（要 native 基準）；
測試 host 內部的東西留 hosttest／dev 固件。

| 組件 | 新位置 | 處置 |
|---|---|---|
| `hw_gpio` | `components/tasks/hw_gpio` | 正式（留固件） |
| ncm（前 `test_mod_tasks`：loader／WAMR engine） | `components/host/ncm` | 外掛宿主；`NC_ENABLE_NCM` 開關（tests 自動開；release 預設冇、亦唔 fetch WAMR） |
| `test_action`（0xF0xx 55 條＋0xF050） | `components/dev/test_action` | 留 dev 固件（開發／量測工具，唔係任務） |
| `test_bench_a/b/c` | `components/dev/test_bench_tasks` | 效能測試 → 留 dev 固件（native 基準） |
| `test_rgb_led_matrix` | `components/dev/test_rgb_led_matrix_tasks` | 效能／視覺測試 → 留 dev；pattern 可外掛（T-NCM-010） |
| `test_tcp` | `components/dev/test_tcp_tasks` | 效能（吞吐）→ 留 dev 固件 |
| `test_hello` | **已刪（2026-11-17）** | 功能測試改用外掛 `hello_task.nct` |
| `test_probe` | **已刪（2026-11-17）** | 測 host hub 內部；由 hosttest `hub_test` ＋ `0xF003 HUB_BENCH` 覆蓋 |

## 3.1 任務模組編譯工具

`tools/ncm_build.py`（2026-11-17）：
- `python3 tools/ncm_build.py` → 編譯 `tools/ncm_samples/` 全部；
- `python3 tools/ncm_build.py hello_task --device` → 編譯＋複製到 MSC `tasks/`；
- 每個任務用 `<name>.build.json` 描述 fps／budget／mem／stack…，`<name>.meta.json` 描述名稱；
- WAT 用 `wat2wasm`；C 路徑預留（clang wasm32＋wasm-ld）；
- 取代舊 `tools/ncm_samples/build.sh`。

## 4. 更新方式

1. 每次驗證完，喺 §1 追加或更新一行（保留舊數字）。
2. 升層要引用「module-adoption §5」的量測門檻，並記錄升層原因。
3. 收納入固件（L2）要同時更新 ABI／文件／owner，並保留 L0 版本作 golden。
