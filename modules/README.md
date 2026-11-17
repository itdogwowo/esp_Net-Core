# modules/ — 外掛任務（唔屬於 firmware 專案）

> **原則**：外掛任務（`.nct`／`.nce`）由工具獨立編譯，**唔進入 firmware 的原始碼樹**。
> firmware 只負責載入＋執行；任務本身係可換的資料／機器碼。
> 命名規範見 [naming.md §7](../docs/03_notes/naming.md)；收納層級見
> [module-adoption](../docs/03_notes/module-adoption.md)。

## 命名規則（由路徑一眼識別）

```
modules/
├── tasks/                      任務模組（輸出 .nct，kind=task）
│   ├── test_<name>/            測試／量測用（dev only，唔會收納入固件）
│   ├── demo_<name>/            示範（可展示，未到 production）
│   └── <name>/                 真任務（production 候選）
├── effects/                    效果模組（輸出 .nce，kind=effect；未來）
│   ├── test_<name>/ ...
│   └── <name>/ ...
└── out/                        編譯產物（.wasm／.nct／.nce；gitignore）
```

每個模組資料夾：

```
modules/tasks/test_hello/
├── test_hello.wat      源碼（.wat；或 .c／.wasm）
├── build.json          fps／budget／mem／stack／cmds…（可選）
└── meta.json           name／desc／usage（可選）
```

- 資料夾名 = 模組名 = payload 檔名前綴；`test_` = 測試，`demo_` = 示範，無前綴 = 真任務。
- 副檔名由 role 決定：`tasks/` → `.nct`、`effects/` → `.nce`（build.json 可覆寫 kind）。
- **冇 `.nca`**：指令（action）係固件 C 代碼，唔係可載入模組；外掛要加自己的
  指令，係喺 `build.json` 用 `"cmds": [0x3xxx]` 宣告（vendor 區 0x3000–0x3FFF）。

## 編譯

```bash
python3 tools/ncm_build.py                          # 全部 → modules/out/
python3 tools/ncm_build.py test_hello               # 只編一個
python3 tools/ncm_build.py --list                   # 列出 role／kind
python3 tools/ncm_build.py test_hello --device      # 編譯＋裝到 /Volumes/NC-*/tasks
python3 tools/ncm_build.py --dir ~/my-mods          # 外部任務目錄（獨立專案）
```

`--dir` 指向任何同結構（或 flat `<name>/`）的目錄 → 外掛任務可以住在另一個 repo，
唔使同 firmware 同一個專案。

## 現有模組

| 路徑 | 模組名 | 類型 | 用途 | 量測 |
|---|---|---|---|---|
| `tasks/test_hello/` | `test_hello` | 測試（smoke） | 1 Hz 印字＋讀／反轉 GPIO21 | — |
| `tasks/test_bench/` | `test_bench` | 測試（效能） | wasm 直譯 vs 原生 C（50k LCG） | 24.9 ms vs 1.68 ms（14.8×） |
| `tasks/test_api_cost/` | `test_api_cost` | 測試（量測） | API 呼叫成本 | ≈2.0–2.4 µs/call |

## 對外 ABI

見 `firmware/components/host/ncm/include/nc_mod_abi.h`：export `task_tick`（必需）
／`task_init`（可選），import 白名單 `nc_print*`／`nc_gpio_*`／`nc_now_us`。
