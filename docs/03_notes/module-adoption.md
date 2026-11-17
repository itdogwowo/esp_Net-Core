# 模組收納（module adoption）：測試 → 外掛驗證 → 分層收納

> **狀態：定案（2026-11-17）**。相關：[ncm-loader.md](ncm-loader.md)、
> [ncm-contract.md](ncm-contract.md)、[test-register](../00_pm/test-register.md)、
> [status §7.82／§7.83](../00_pm/status.md)。

---

## 1. 一句話

> **所有新功能／測試，首先做成外掛模組（通用 `.nct`）喺 tests 固件驗證；
> 測試冊驗收通過之後，才按「量測到嘅性能需要」分層收納入正式固件。**

唔會一開始就寫入固件；亦唔會為咗外掛而外掛。每一層升級都要有數字支持。

## 2. 四層收納階梯

| 層 | 形態 | 位置／格式 | 代價（相對 C） | 適合 |
|---|---|---|---|---|
| **L0** | 外掛 wasm（直譯） | `.nct`，`fmt=wasm`；`/data/tasks`、`/data/pixel/effects` | **≈15× 慢**（S3 實測，見 §5） | 邏輯／pattern／狀態機／協議／GPIO 編排 |
| **L1** | 外掛 wasm（AOT） | 同一個 `.nct`，`fmt=aot`＋`target_id`（S3=xtensa、P4=rv32f） | 預期 **2–4× 慢**（待 P3 實測） | 性能敏感、但仍然要「部署後可換」 |
| **L2** | 原生 C 固件組件 | `firmware/components/...`（唔再係外掛） | **1×**（基準） | 熱路徑、驅動、timing、buffer、實時性 |
| **L3** | 硬件加速 | DMA／PPA（P4）／RMT／IRAM placement／assembly | 可能更快／省 CPU | L2 都仍然唔夠（先量，後做） |

- 升級係**單向按需要**：L0 夠用就停喺 L0；只有量到 budget 爆或穩定性問題才升。
- 每升一層，**保留上一層做 golden reference／回退**（行為一致先算收納）。
- AOT 唔會逃出白名單：佢只係快啲嘅 guest，唔算「加能力」。

## 3. 通用格式（唔會每層換格式）

- 容器永遠係 `.nct`（48 B header＋manifest＋可選 cmd/meta＋payload）；L0/L1 只係
  payload `fmt` 同 `target_id` 唔同。
- 能力（native API）以 **capability provider** 形式加：每一組一個組件
  （例如 `nc_cap_gpio`、`nc_cap_time`、`nc_cap_led`），自己註冊入
  `NativeSymbol` 表；Kconfig／config 決定要邊幾組。外掛 manifest 用
  `requires` 聲明，`nc_res_check()` 擋。
- **唔可以**由 FAT 載入 native code／把 AOT 當 plugin：AOT 仍然只可以 import
  白名單；native code 要 executable RAM＋冇 MMU 保護 → 一錯就磚機。原生能力
  只可以經 **OTA 固件更新**（十幾秒、唔使插線）。

## 4. 收納準則（admission gates）

一個外掛要收納（或升層）之前，測試冊要齊：

1. **行為**：golden 輸入／輸出、邊界條件、失敗模式（trap、超時、資源不足）。
2. **性能**：板上實測 per-tick 時間、flash／內部 heap／PSRAM／stack 用量；
   同當前層既有限額比較（見 §5）。
3. **安全**：只用白名單 capability；guest 指標全部 validate；無 blocking call；
   GPIO 等有腳位白名單。
4. **回退**：保留上一層版本作對照；L2 收納後，L0/L1 版本留作 golden。
5. **文件**：ABI／用法／owner 更新；測試冊有一行、test id 可追。

## 5. 性能門檻（2026-11-17 首次實測，S3 Mini-Nano）

同一個 workload（50,000 次 LCG，`v = v*1664525 + 1013904223`，checksum 相同）：

| 形態 | 實測 | 備註 |
|---|---|---|
| **L2 原生 C**（新測試指令 `0xF050 CPU_BURN`） | **1,683 µs**（5 次：1,677–1,687） | 6–8 cycles/iteration @240 MHz |
| **L0 wasm 直譯**（`tools/ncm_samples/bench_task.wat`） | **24,887 µs**（5 次：24,868–24,910） | 每次 tick 用 `nc_now_us()` 自量 |
| **倍數** | **≈14.8×** | 同一塊板、同一時段；有一個 335 ms outlier（USB 擷取期間，不計） |

建議門檻（暫定，之後按實測校準）：

- 外掛 tick 用咗 **> 30% 的 budget_us** → 考慮升 **L1 AOT**；
- AOT 之後仍然 **> 30%** → 升 **L2 原生 C**；
- L2 仍然唔夠 → 才入 **L3**（先量 DMA／IRAM／PPA 等，見
  [measurement-method](../02_guides/measurement-method.md)）。

> 註：呢個 workload 係純 ALU；PSRAM 影響細（locals 喺 internal exec stack）。
> 大 framebuffer／streaming 類 workload 嘅 PSRAM 成本要另外量（buffer 管理器
> 只解決 host 側搬資料，唔解決 guest linear memory 喺 PSRAM）。

## 6. 測試喺邊度跑

| 層 | 測試位置 |
|---|---|
| L0／L1 | **tests 固件**（`NC_ENABLE_TESTS=ON`）＋外掛模組＋CDC log；PC 先跑 `wasmtime` stub（待做） |
| L2 | `tools/hosttest/`（唔上板）＋ dev 固件 bench 指令（`0xF001`／`0xF01C`／`0xF050`…）＋對比 L0/L1 |
| L3 | [measurement-method.md](../02_guides/measurement-method.md) 的程序（同一 binary 三次、環境固定） |

## 7. 唔做

- 由 `/data` 載入 native code 或把 AOT 當「新能力」。
- 把所有測試一次過搬入外掛——測試 host 內部（hub／buf／flash／OTA／partition）
  嘅嘢留 hosttest／dev 固件（搬去 wasm 只會測到 WAMR，見 §7.82）。
- 外掛模組直接開 raw register／host pointer。

## 8. 現況與下一步

- 已達 **L0**：`hello_task`（列印＋GPIO 讀／反轉）＋`bench_task`（性能對照）；
  能力白名單 7 個 import（print／print_i32／print2／gpio_read/write/toggle／now_us）。
- 已量：interp ≈15× C（上面）；**AOT 未接，未有板上數字**。
- 下一步：
  1. capability provider 重構（`nc_cap_*`，先 gpio/time）；
  2. `nc_cap_led`（`nc_led_show/info`）→ LED pattern 任務 L0 → L2（driver 留 host）；
  3. **AOT spike**（P3）：`wamrc` 編 Xtensa AOT、S3 PSRAM dual-bus 執行路徑、量 L1；
  4. per-module loop／prio／core（manifest 已有欄位，loader 只 attach 未調度）；
  5. 測試冊（[test-register](../00_pm/test-register.md)）逐項更新。
