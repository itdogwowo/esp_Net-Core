# ncm 樣本模組（外部任務）

最小可上板的 `.nct` 樣本：**列印 + 讀 GPIO + 反轉 GPIO**，用來驗證
「檔案系統 → 掃描 → 建表 → attach → 實例化 → 定時 tick」整條鏈。

| 檔 | 是什麼 |
|---|---|
| `hello_task.wat` | 模組原始碼（WebAssembly text） |
| `hello_task.meta.json` | meta 區塊（name／desc／usage／provides） |
| `build.sh` | `wat2wasm` + `tools/ncm.py pack` → `out/hello_task.nct` |

## 跑

```bash
tools/ncm_samples/build.sh          # 需要 wat2wasm
cp tools/ncm_samples/out/hello_task.nct /Volumes/NC-xxxx/tasks/
python3 tools/nc4.py --port /dev/cu.usbmodem* reboot
python3 tools/nc4_log.py --port /dev/cu.usbmodem*
```

預期 CDC log（1 Hz）：

```
ncm.wamr: [hello_task.nct] tick pin21 read=1 toggle=0
ncm.wamr: [hello_task.nct] tick pin21 read=0 toggle=1
...
```

## bench_task：性能對照

`bench_task.wat` 用同一個 LCG workload（50,000 次）量 wasm 直譯 vs 原生 C：
- 外掛版：`task_tick` 用 `nc_now_us()` 自量，印 `bench iters=50000 us=… sum=…`；
- 原生版：NC4 `0xF050 CPU_BURN`（同樣 iters），回 `{"iters":…,"us":…,"sum":…}`。

2026-11-17 S3 實測：C 1,683 µs、wasm 直譯 24,887 µs（≈14.8×），checksum 一致。

## 模組 ABI

見 `firmware/components/tasks/test_mod_tasks/include/nc_mod_abi.h`：任務模組
export `task_tick`（必需）／`task_init`（可選），import 白名單七個
`env.nc_*`（列印、print2、讀／寫／反轉 GPIO、`nc_now_us`）。GPIO 只准安全腳位
（USB／flash／UART／strap 一律拒絕）。收納層級見
[module-adoption](../../docs/03_notes/module-adoption.md)。
