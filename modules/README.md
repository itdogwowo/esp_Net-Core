# modules/ — 外掛任務（唔屬於 firmware 專案）

> **原則**：外掛任務（`.nct`）由呢個工具獨立編譯，**唔會進入 firmware 的原始碼樹**。
> firmware 只負責載入＋執行；任務本身係「可換的資料／機器碼」，唔係固件一部分。
> 收納層級／性能門檻見 [module-adoption](../docs/03_notes/module-adoption.md)。

## 結構

```
modules/
├── <task-name>/           一個任務一個資料夾（獨立可搬走）
│   ├── <task-name>.wat    源碼（或 .c／.wasm；C 要 clang wasm32＋wasm-ld）
│   ├── build.json         fps／budget／mem／stack…（可選）
│   └── meta.json          name／desc／usage（可選）
└── out/                   編譯產物（.wasm／.nct；gitignore）
```

## 編譯

```bash
python3 tools/ncm_build.py                  # 全部任務 → modules/out/
python3 tools/ncm_build.py hello_task       # 只編一個
python3 tools/ncm_build.py --list           # 列出任務
python3 tools/ncm_build.py hello_task --device   # 編譯＋裝到 /Volumes/NC-*/tasks
python3 tools/ncm_build.py --dir ~/my-mods  # 用外部任務目錄（完全獨立專案）
```

`--dir` 讓工具指向任何裝住 `<name>/` 資料夾的目錄 → 外掛任務可以住在另一個 repo，
唔使同 firmware 同一個專案；編好的 `.nct` 直接放入裝置 `/data/tasks/`。

## 現有任務

| 任務 | 用途 | 量測 |
|---|---|---|
| `hello_task` | L0 golden：1 Hz 印字＋讀／反轉 GPIO21 | — |
| `bench_task` | wasm 直譯 vs 原生 C 同 workload（50k LCG） | 24.9 ms vs 1.68 ms（14.8×） |
| `api_cost` | API 呼叫成本（`nc_now_us`／`nc_gpio_read`） | ≈2.0–2.4 µs/call |

## 對外 ABI

見 `firmware/components/host/ncm/include/nc_mod_abi.h`：export `task_tick`（必需）
／`task_init`（可選），import 白名單 `nc_print*`／`nc_gpio_*`／`nc_now_us`。
