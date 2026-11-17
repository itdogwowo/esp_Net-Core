# 模組收納（module adoption）：測試 → 外掛驗證 → 分層收納

> **狀態：定案（2026-11-17）**。相關：[ncm-loader.md](ncm-loader.md)、
> [ncm-contract.md](ncm-contract.md)、[test-register](../00_pm/test-register.md)、
> [status §7.82／§7.83](../00_pm/status.md)。

---

## 1. 一句話

> **所有新功能／測試，首先做成外掛模組（通用 `.nct`）喺 tests 固件驗證；
> 測試冊驗收通過之後，才按「量測到嘅性能需要」分層收納入正式固件。**

唔會一開始就寫入固件；亦唔會為咗外掛而外掛。每一層升級都要有數字支持。

## 2. 收納階梯（四層＋一個可選層）

| 層 | 形態 | 位置／格式 | 代價（相對 C） | 沙箱 | 適合 |
|---|---|---|---|---|---|
| **L0** | 外掛 wasm（直譯） | `.nct`，`fmt=wasm`；`/data/tasks`、`/data/pixel/effects` | **≈15× 慢**（S3 實測，見 §5） | ✅ | 邏輯／pattern／狀態機／協議／GPIO 編排 |
| **L1** | 外掛 wasm（AOT） | 同一個 `.nct`，`fmt=aot`＋`target_id`（S3=xtensa、P4=rv32f） | 預期 **2–4× 慢**（待 P3 實測） | ✅ | 性能敏感、但仍然要「部署後可換」 |
| **L1.5**（可選） | **native ELF／`.so`**（Espressif `elf_loader`） | 由 `/data` 載入 `.elf`／`.so`（S3 可在 PSRAM 執行） | **≈1×**（PSRAM 取指另計） | ❌ **完全受信** | 要 C 速度 **而且**要模組級更新 → **只限自己寫／簽名** |
| **L2** | 原生 C 固件組件 | `firmware/components/...`（唔再係外掛） | **1×**（基準） | ❌（但係你自己 code） | 熱路徑、驅動、timing、buffer、實時性 |
| **L3** | 硬件加速 | DMA／RMT／LCD_CAM／I2S／PPA／JPEG／ISP（見 [goal](goal-hardware-acceleration.md)） | 更快／省 CPU | ❌ | L2 都仍然唔夠（先量，後做） |

- 升級係**單向按需要**：L0 夠用就停喺 L0；只有量到 budget 爆或穩定性問題才升。
- 每升一層，**保留上一層做 golden reference／回退**（行為一致先算收納）。
- AOT 唔會逃出白名單：佢只係快啲嘅 guest，唔算「加能力」。

## 2.5 沙箱與信任：wasm／AOT／native ELF

| 保證 | wasm L0／L1 | native ELF（L1.5） |
|---|---|---|
| **記憶體隔離** | 只有自己 linear memory；每次 access bounds check；指唔到 host | 同 firmware 同一個 address space；wild pointer 可以改任何記憶體 |
| **能力白名單** | 只可以叫 `env.nc_*`；host 先 validate app addr | 只可以叫 symbol table 內的函數，但**記憶體安全冇咗**（冇 MMU／check） |
| **失敗隔離** | trap → 標 `bad`、live=NULL，核心照跑 | crash = panic／reboot；開機自動載入就 boot loop（USB 未起，要按 BOOT） |

「失去沙箱」唔係「慢咗」咁簡單，係**信任模型改變**：模組變成 firmware 一部分，
可以（意外或惡意）讀寫 flash／heap／其他 module、直接寫 GPIO 暫存器、令系統死機。
所以 L1.5 **只可以俾自己寫／簽名嘅 module**；第三方永遠留在 L0／L1。

> 注：**AOT（L1）仍然有沙箱**——佢係「沙箱內嘅機器碼」，所以係 2–4× C；
> 想 1× C 就一定要犧牲沙箱（L1.5／L2）。

### 2.5.1 L1.5 用官方工具，唔使自己寫 linker

Espressif 有官方 component
[`espressif/elf_loader`](https://components.espressif.com/components/espressif/elf_loader)
（esp-iot-solution 的一部分）：由 filesystem 載入 ELF／`.so`，做 relocation、
symbol 解析、`dlopen/dlsym/dlclose` 風格介面；README 明確支援
**ESP32-S3／S2／P4／C6／C61 在 PSRAM 執行 ELF**，有 libc／IDF／customer
symbol table 同 `esp_elf_register_symbol()`；配套 examples：
`idf.py elf`（獨立 `.elf`）、`project_so`（`.so`）、`elf_console_example`。

所以「自己寫一個 OS 級動態連結器」**唔係唯一路；亦唔係首選**：

- 唔使寫：loader、relocation（Xtensa `R_XTENSA_32`／`l32r`）、bus mirror、cache sync；
- 仍然要做：**收窄 symbol table**（只 export `nc_*` 白名單，唔要完整 libc／IDF）、
  ABI version 檢查、**簽名／allowlist**、boot safe mode、同 firmware OTA 的版本配套、
  量 PSRAM 取指速度、任務管理器整合（attach 租戶、call tick）。
- 工程估算：官方 component 之下，一個「trusted native plugin」PoC 由幾星期降到
  **幾日**（build workflow＋ABI 白名單＋簽名＋測試）。但**安全邊界／ABI 耦合／
  boot crash 風險係本質，唔會因為有 AI 幫手而消失**。

### 2.5.2 為何唔自己寫動態連結器

- S3 冇 MMU（只有 MPU，作唔到 fine-grained sandbox）；native code 一 wild 就死；
- Xtensa relocation／cache／PSRAM bus mirror 全部要自己處理（官方 loader 已做）；
- ABI 綁 firmware：每次 OTA 都可能令舊 module 壞，要 freeze ABI＋版本檢查；
- 多架構（S3 xtensa／P4 RISC-V）各一套；
- 結論：**用官方 component 做 PoC 可以，自己寫就唔值**；而且無論邊種做法，
  都唔會令 native module 變返「安全」。

### 2.5.3 討論紀錄：MicroPython viper vs AOT

- `@micropython.viper` 係 **runtime native emitter**：function 定義時由 firmware
  內建 compiler 即場編成機器碼，放可執行 RAM（ESP32 係 IRAM）；`int` 係機器字、
  `ptr8/16/32` 係原始位址、冇 bounds check → 快，但**只可以信自己 code**。
- `mpy-cross`／`.mpy` 只係 source→bytecode，唔會變機器碼；「MPY 接近 C」主要係
  viper／`@native` ＋ C module 嘅效果，唔係 AOT。
- 我們嘅 **L1 AOT = 離線編機器碼**：同一速度級（預期 2–4× C），但唔使 firmware
  帶 compiler、唔使 runtime IRAM code cache、語言係完整 wasm、仍有沙箱。
- 兩者限制（IRAM／語言 subset／冇安全）係本質；**唔等於 C**，真 C 只有 L2，
  硬件加速係 L3。詳情見 [goal-hardware-acceleration.md](../00_pm/goal-hardware-acceleration.md)。


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

- **默認唔用** native ELF 外掛（L1.5）：只可以係可信／簽名 module 嘅可選方案；
  第三方 plugin 永遠 L0／L1。
- 由 `/data` 載入原生 code 當「新能力」；AOT 亦唔算加能力（仍然白名單）。
- 把所有測試一次過搬入外掛——測試 host 內部（hub／buf／flash／OTA／partition）
  嘅嘢留 hosttest／dev 固件（搬去 wasm 只會測到 WAMR，見 §7.82）。
- 外掛模組直接開 raw register／host pointer。

## 8. 現況與下一步

- 已達 **L0**：`hello_task`（列印＋GPIO 讀／反轉）＋`bench_task`（性能對照）＋
  `api_cost`（API 成本量測）；能力白名單 7 個 import
  （print／print_i32／print2／gpio_read/write/toggle／now_us）。
- **2026-11-17 重組**：功能測試（hello／probe）已由固件移出（刪 component，改外掛
  `.nct` 驗證）；效能測試留 `components/dev/`；宿主 ncm 搬到 `components/host/ncm`
  並有 `NC_ENABLE_NCM` 開關（release 預設唔開、唔 fetch WAMR）。
- **API 成本實測**：`nc_now_us` ≈2.0 µs/call、`nc_gpio_read` ≈2.4 µs/call
  （S3、N=2000，空迴圈底 0.36 µs/iter）→ 每 tick／每 frame 叫幾次可忽略；
  每 pixel／每 element 叫就係災難（要 batching）。見測試冊 T-NCM-017。
- 已量：interp ≈15× C（上面）；**AOT 未接，未有板上數字**。
- 終極目標：**L3 硬件加速**（DMA／RMT／LCD_CAM／I2S／PPA／JPEG／ISP），
  里程碑 M1–M5 見 [goal-hardware-acceleration.md](../00_pm/goal-hardware-acceleration.md)。
- 下一步（照里程碑）：
  1. **M2 AOT spike**（P3）：`wamrc` 編 Xtensa AOT、S3 PSRAM dual-bus 執行、量 L1；
  2. capability provider 重構（`nc_cap_*`，先 gpio/time）；`nc_cap_led`；
  3. **M4 第一條 L3 pipeline：S3 RMT LED matrix**（fps/CPU before-after）；
  4. per-module loop／prio／core（manifest 已有欄位，loader 只 attach 未調度）；
  5. L1.5 native ELF 只作為「可選 PoC」（有需要＋簽名信任模型齊備先做）；
  6. 測試冊（[test-register](../00_pm/test-register.md)）逐項更新。
