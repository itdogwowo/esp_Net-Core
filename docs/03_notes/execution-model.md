# 執行模型：FreeRTOS 每 loop 一條 task vs 每核合作式大循環

> **用途**：回答「舊專案是設計 B，C 版是 A，FreeRTOS 是不是必然更好，能不能小改裝兼容兩邊」。
> **結論先講**：可以兼容，而且改裝不大（約 200 行、4 個檔），但**「小」不等於「無代價」**——
> 有 5 個邊界條件一定要處理，否則會出現「停不掉、量不準、stack 不夠」的隱形 bug。
>
> 相關：[execution-rules.md](execution-rules.md)（搶佔條件、等待規則、核心隔離——**先讀這份**）、
> [core-round.md](core-round.md)（大循環量測，已停用）、[task-lifecycle.md](task-lifecycle.md)、
> [task-model.md](task-model.md)。

---

## 0. 一句話

- **A（現況）**：每條 loop = 一條 FreeRTOS task。適合會阻塞、需要優先序、需要低延遲的路徑。
- **B（舊專案）**：每核一條執行緒，依序跑完所有 task。適合全部短工作、追求確定性與省 RAM。
- **兩邊兼容**：在 loop 上加一個 `group` 欄位。`group = 0` 就是 A；`group = N` 就由同一條
  FreeRTOS task 依序跑（局部 B）。**同一個 codebase、同一個管理器、同一個量測。**

---

## 1. 舊專案確實是 B（證據）

`lib/sys/task_manager.py::runner_loop(core_id)`：

```python
while True:
    now_ms = time.ticks_ms()
    ... WDT 每圈餵一次 ...
    if self._dirty[core_id]:
        self._update_tasks(core_id)
    current_tasks = list(active_list)
    for task in current_tasks:
        t0 = time.ticks_us()
        task.loop()                 # 依序、不搶佔
        task.perf["loop_us"] = ...
    time.sleep_ms(0)                # 一輪結尾讓出
```

- core0 = 主線程；core1 = `_thread.start_new_thread`。
- 每個 task 的 `loop()` 必須**非阻塞**（`net_bus.poll()` 輪詢、`time.sleep_ms(0)` 讓出）。
- 大循環、per-task 量測、WDT、boot layer、run_once 全部是**同一層**的東西。

---

## 2. 兩種模型換到了什麼

| | **B：每核一個大循環** | **A：每 loop 一條 task** |
|---|---|---|
| 阻塞的影響 | **整核停住** | 只停那條 task |
| 喚醒延遲 | 最多一個 round；或自旋吃 100% CPU | notification 即時（RTT 3.5 ms 靠這個） |
| 優先序 | 沒有，次序固定 | 有（decode 4 / tcp_rx 10 / httpd 10） |
| 每任務記憶體 | 共用一個 stack | 每條 6 KB（S3 目前 5 條 ≈ 23.5 KB） |
| 共享狀態 | 單線程，不用鎖 | 要小心跨 task／跨核 |
| 大循環 | 天然存在 | 要用 `nc_epoch` 合成（已做） |
| 錯誤隔離 | 一個 task 例外可 catch | 一條 task crash 可能整機重啟 |
| 工具 | 自己寫 counter | FreeRTOS stack HWM、TWDT、trace |

---

## 3. FreeRTOS 是不是必然更好？—— 不是

| 問題 | 偏向 B | 偏向 A |
|---|---|---|
| 工作會不會**阻塞**？（socket / queue / flash / I2S） | 不會，純短工作 | 會，或用阻塞 API |
| 需要**不同優先序**？ | 不需要 | 需要（急 decode vs 慢 log） |
| 需要**高吞吐／低延遲**？ | 不需要 | 需要（現在 7.4 MB/s、RTT 3.5 ms） |
| 記憶體很緊？ | 很緊 | 有餘裕 |

現實限制：**IDF 本身（lwIP、httpd、esp_timer、USB）全部是 task**，
所以「純 B」在 C/IDF 上做不到。能選的只有「**自己的 loop** 用 A 還是 B」。

### 3.1 對現有 loop 的判斷

| Loop | 特性 | 適合 |
|---|---|---|
| decode0 / decode1 | 阻塞在 notification；8 KB 幀、高效能 | **A** |
| tcp_rx / httpd / lwIP | 阻塞式 socket；IDF 建 task | **A（必然）** |
| UDP | 阻塞 `recvfrom` | A（或改非阻塞才可能 B） |
| fx | 自己 delay、30–50 Hz | A 或 group 都可 |
| hello / probe | 低頻、短工作 | **group（B）最適合** |
| bench_a/b/c | 固定工作量、5 ms | **group（B）最適合** |

---

## 4. 「小改裝兼容兩邊」的設計

### 4.1 API：加一個欄位

```c
typedef struct {
    ...
    uint32_t period_ms;
    uint32_t budget_us;
    bool run_once;
    uint8_t group;      // 新增：0 = 自己一條 task；>0 = 同核同 group 合成一條
    bool (*enabled)(void);
} nc_task_loop_t;
```

- 所有 initializer 都是 designated，**加欄位不會弄壞既有宣告**（預設 0 = A）。
- `group` 的號碼只在同一核內有意義。

### 4.2 執行語義

```text
group = 0   → 現在的 nc_task_rt_start()：一條 task 跑一條 loop（A）
group = N   → 同核、同 N、同 layer 的 loop 合成一個「group runner」：
                一個 FreeRTOS task
                stack = 成員最大值
                priority = 成員最大值
                period = 成員共用的 period（必須一致）
                每輪：for member: if enabled { measure; fn(); }  → delay(period)
```

- 成員**必須非阻塞**（這是 B 的前提，無法用框架強制，只能寫進規則 + 文件）。
- 成員的 `budget_us`、`count/last/max`、`over` 照舊各自記。
- epoch：每個成員跑完之後照樣 `nc_epoch_mark()`；group runner 只是驅動者，
  **大循環的語義完全不變**（一輪 = 同核所有 loop 各跑一次）。

### 4.3 五個一定要處理的邊界條件（這就是「小」的代價）

| # | 問題 | 處理 |
|---|---|---|
| 1 | **成員 period 不一致** | 建 group 時驗證；不一致 → log 警告 + 該成員退回自己一條 task（不要靜默合併） |
| 2 | **成員不同 layer** | 同上：group 只在同一 layer 內合併（否則 stack/生命週期對不上） |
| 3 | **stack 要幾多** | 取成員最大值；之後加入的成員若更大 → 拒絕並倒退成獨立 task（stack 已配好，改不了） |
| 4 | **`nc_task_exit_requested()` 失效** | 目前用「目前 task 的 handle」反查 rt；group 內所有成員共用一個 handle。要改成：runner 呼叫 `fn` 前設定「目前成員」指標，`nc_task_rt_exit_requested()` 讀它（每核一份，用 `xTaskGetCoreID()` 索引） |
| 5 | **停止語義** | `nc_task_rt_stop(member)` 只設該成員 `exit_req`；group runner 要等**所有**成員都 `exit_req` 才退出，退出時把全部成員標成 `exited` |

其他：`run_once` 的成員不淮入 group（它跑完就沒了，會令 group 的剩餘成員失去驅動）；
`enabled()` 的 `false` 成員仍然留在 group（跳過 fn）。

### 4.4 生命週期

```text
第一個成員 start    → 建立 group task（stack/prio 定案）
之後的成員 start    → 加入成員表（驗證 period/layer/stack）
成員 stop           → 只設 exit_req
全部成員 exit_req   → group runner 跑完這一輪 → 退出 → 全部成員 exited
```

### 4.5 WDT（B 最有價值的副產品）

- **group runner**：一圈餵一次（跟舊版一樣）。
- **獨立 task（A）**：用 IDF 的 TWDT per-task 註冊，或用下面第 5 節的 round monitor。
- 注意：TWDT 是**全域**的（所有註冊 task 都要餵）；不要一部分註冊、一部分不註冊。

---

## 5. 更小的第一步：round watchdog（唔需要 group）

如果只想拿回 B 的「卡死偵測」，唔需要改執行模型：

```text
每核一個 monitor（可以係 slow_worker 的一部分）：
    每 N 秒比較 nc_epoch 的 rounds 有沒有增加
    停滯 → ESP_LOGE + 記錄當下各 loop 的 count/last_us（找出卡在哪）
    可選：停滯超過 M 秒 → esp_restart()（或餵 TWDT 讓它自然重置）
```

成本：約 50 行、無新 task、無 API 變更。**建議先做這個**，因為它直接回答
「系統有沒有卡死」，而且不影響任何執行路徑。

---

## 6. 實作清單（如果要 group）

| 改動 | 檔案 | 估計行數 | 風險 |
|---|---|---:|---|
| `group` 欄位 + 驗證 | `nc_task.h`、`nc_task.c` | ~25 | 低 |
| group runner（成員表、trampoline、生命週期） | `nc_task_loop.c` | ~150 | 中 |
| 「目前成員」指標（修 `exit_requested`） | `nc_task_loop.c` | ~20 | 中 |
| epoch 不變（已支援多 loop 同核） | — | 0 | — |
| host test：group 的 start/stop/enable/epoch | 新 `group_test` 或擴充 `task_test` | ~120 | 低 |
| 文件（本文件） | `docs/03_notes/` | — | — |

**驗收**：
1. `group = 0` 的行為與現在**完全一致**（回歸：現有 8 支 host test 不變）。
2. 同 group 兩條 loop：只有一條 FreeRTOS task、stack 只配一次、次序固定。
3. 停用其中一條：另一條照跑，round 仍然完成。
4. 停止其中一條：group 要等另一條都停才退出（不可 mid-round 消失）。
5. period 不一致的 group：開機 log 明確講「退回獨立 task」。

---

## 7. 不建議做的事

- **不要把 decode/tcp_rx/httpd 拉進 group**：它們會阻塞，會拖死整核。
- **不要為咗「統一」而全部改 B**：要重寫非阻塞 socket、改 WDT、改量測，
  而最需要的低延遲/吞吐會退步。
- **不要用「round barrier」去模擬 B**：多一層同步、沒有 stack 節省，比 group 更複雜。
- **不要讓 group 成員數無上限**：建議每核最多 4 個 group、每 group 最多 6 條。
  （目前每核最多 8 條 loop，epoch 的 token 也是 8。）

---

## 8. 結論與建議順序

1. **保留 A 作為 I/O 邊界**（decode、TCP、UDP、httpd）。
2. **先做 round watchdog**（~50 行）：拿回 B 的卡死偵測，不改執行模型。
3. **有需要才做 group**：當低頻 loop（sensors、LED、UI、bench）多起來，
   把它們合併成一條 task，省 stack 並且順序確定。
3. 如果你的目標是「同一個 codebase 兩邊都行」——可以，`group=0/1` 就是開關；
   但「兩邊都行」不等於「兩邊都一樣好」，decode 永遠應該留在 A。

> **一句話**：舊 B 在 MicroPython 是對的；C/IDF 上 A 是現實。
> 用 `group` 把 B 局部取回是合理的「小改裝」，但真正值得先做的是
> **round watchdog** —— 它用 50 行就拿回 B 最有價值的功能。
