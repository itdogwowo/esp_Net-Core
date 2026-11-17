# modules/ — 外掛模組（唔屬於 firmware 專案）

> **原則**：外掛模組由工具獨立編譯，**唔進入 firmware 的原始碼樹**。
> firmware 只負責載入＋執行。命名見 [naming.md §7](../docs/03_notes/naming.md)。

## 命名規則（睇路徑就知係乜）

**每個 leaf 名都要以角色結尾**（任務 `_tasks`、指令 `_actions`、效果 `_effects`），
同時放入同名角色目錄：

```
modules/
├── tasks/                     任務模組       → out/<name>.nct（kind=task）
│   ├── test_hello_tasks/      測試（smoke）
│   ├── test_bench_tasks/      測試（效能）
│   ├── test_api_cost_tasks/   測試（量測）
│   ├── demo_<name>_tasks/     示範
│   └── <name>_tasks/          真任務（production 候選）
├── actions/                   指令模組       → out/<name>.nct（kind=normal，
│   └── <name>_actions/         build.json 必須有 "cmds": [0x3xxx]）
├── effects/                   效果模組       → out/<name>.nce（kind=effect）
│   └── <name>_effects/
├── lab/                       實驗／量測模組 → out/<name>.nct（leaf 以 _lab 結尾；
│   ├── bench_compare_lab/       一次性驗證，唔係任務、唔會收納）
│   └── api_cost_lab/
├── build/                     中間產物（.wasm；gitignore，唔會混入成品）
└── out/                       最終成品（只有 .nct／.nce；一入去就見到要收集嘅嘢）
```

- `test_`（測試／量測，dev only，唔會收納入固件）／`demo_`（示範）／無前綴（真任務）。
- 每個模組資料夾：`<name>/`＋`<name>.wat`（或 `.c`／`.wasm`）＋`build.json`＋`meta.json`；
  資料夾名＝payload 檔名＝模組名，三處一致。
- 副檔名由 role 決定：`tasks`／`actions` → `.nct`、`effects` → `.nce`。

## 編譯

```bash
python3 tools/ncm_build.py                          # 全部 → out/
python3 tools/ncm_build.py test_hello_tasks         # 只編一個
python3 tools/ncm_build.py --list                   # 列出 role／kind
python3 tools/ncm_build.py test_hello_tasks --device   # 編譯＋裝到 /Volumes/NC-*/tasks
python3 tools/ncm_build.py --dir ~/my-mods          # 外部模組根目錄（獨立專案）
python3 tools/ncm_build.py --clean                  # 清空 build/ 同 out/
```

## 現有模組

| 路徑 | 名稱 | 類型 | 用途 | 量測 |
|---|---|---|---|---|
| `tasks/test_hello_tasks/` | `test_hello_tasks` | 測試（smoke） | 1 Hz 印字＋讀／反轉 GPIO21 | — |
| `lab/bench_compare_lab/` | `bench_compare_lab` | 實驗（效能） | wasm 直譯 vs 原生 C（50k LCG） | 24.9 ms vs 1.68 ms（14.8×） |
| `lab/api_cost_lab/` | `api_cost_lab` | 實驗（量測） | API 呼叫成本 | ≈2.0–2.4 µs/call |

> **量測唔係任務**：host 內部量測用 dev 固件 native 指令（`0xF050` 等）；
> 量測「外掛路徑」本身（interp/AOT、API 成本）只係一次性實驗，放 `lab/`，
> 量完就可以移除，唔會收納入固件。

## 對外 ABI

見 `firmware/components/host/ncm/include/nc_mod_abi.h`：export `task_tick`（必需）
／`task_init`（可選），import 白名單 `nc_print*`／`nc_gpio_*`／`nc_now_us`。
