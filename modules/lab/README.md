# modules/lab/ — 實驗／量測模組（唔係任務）

> 放**一次性**驗證用嘅模組：leaf 名以 **`_lab`** 結尾（例如 `bench_compare_lab/`）。
> 呢啲唔係任務、唔會收納入固件；量完就可以移除，數字記錄喺
> [test-register](../../docs/00_pm/test-register.md)。

用途：
- 量「外掛路徑」本身：wasm 直譯 vs AOT、API 呼叫成本、ABI 實驗；
- host 內部量測唔應該用呢度——用 dev 固件 native 指令（`0xF050` 等）。

| 模組 | 用途 | 量測結果 |
|---|---|---|
| `bench_compare_lab/` | 50k LCG：直譯 vs 原生 C | 24.9 ms vs 1.68 ms（14.8×） |
| `api_cost_lab/` | `nc_now_us`／`nc_gpio_read` 呼叫成本 | ≈2.0–2.4 µs/call |
