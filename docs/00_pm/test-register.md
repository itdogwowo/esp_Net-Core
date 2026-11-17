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

## 2. Host 測試（完全唔入固件，L2 工具）

`tools/hosttest/run_all.sh`：**12/12 全過**（2026-11-17）：
`cfg`、`cfgstruct`（含 resources 層）、`ncd`、`task`、`router`、`link`、`mod`、
`hub`、`plan`、`buf`、`portgen`、`ncm`。

## 3. 固件內測試／工具任務處置（2026-11-17 定案）

| 組件 | 處置 |
|---|---|
| `hw_gpio` | 正式（留固件） |
| `test_mod_tasks`（ncm loader／WAMR engine） | 外掛宿主；tests 建置先用，將來升 production（config 可關） |
| `test_hello` | 可外掛（已被 `hello_task` 取代）；留 dev 固件作對照 |
| `test_bench_a/b/c` | 留 dev 固件（測排程；L0 等價版已用 `bench_task` 驗證性能） |
| `test_probe` | 留 dev 固件／hosttest（測 host hub 內部，唔外掛） |
| `test_rgb_led_matrix` | driver 留 host（L2/L3）；pattern 可外掛（T-NCM-010） |
| `test_tcp` | 留 dev 固件（L2）；外掛能力未開放 |
| `test_action`（0xF0xx 55 條） | 留 dev 固件（開發工具，唔係任務） |

## 4. 更新方式

1. 每次驗證完，喺 §1 追加或更新一行（保留舊數字）。
2. 升層要引用「module-adoption §5」的量測門檻，並記錄升層原因。
3. 收納入固件（L2）要同時更新 ABI／文件／owner，並保留 L0 版本作 golden。
