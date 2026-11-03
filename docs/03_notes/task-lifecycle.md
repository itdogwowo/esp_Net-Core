# 任務生命週期：建立 → 啟動 → 開 loop → 停止

> **用途**：回答「一個任務要成立，需要具備什麼條件、要宣告什麼、管理器憑什麼決定要多少記憶體、
> 結束時誰歸還什麼」。這份是**現況**（照 `sys/task` + `sys/task_manager` 的實作寫），不是提案。
>
> 相關：[task-model.md](task-model.md)（房東／租戶的分界）、[writing-a-task.md](writing-a-task.md)（怎麼寫）、
> [memory.md](memory.md)（plan 的細節）

---

## 0. 一句話

**任務 = 一份宣告（`nc_task_t`）＋ 四步流程（登記 → 宣告 → 啟動 → 開 loop）。
管理器不認得任何具體任務；它只讀你的宣告，決定「配多少記憶體、什麼時候呼叫你、什麼時候停你」。**

---

## 1. 建立前要準備什麼

**每個任務都是模型的實例，必須走同一組五個階段：宣告 → 啟動 → 迴圈 → 停止 → 卸載。**
（模型的定義與上游 `class Task` 的對照見 [task-model.md](task-model.md) §2。）

一個任務就是一個 `const nc_task_t`。**只有 `name` 和 `kind` 是必填**，其餘留空就是「什麼都不做」。

| 欄位 | 必填 | 作用 | 沒填會怎樣 |
|---|---|---|---|
| `name` | ✅ | 唯一鍵：slot / `tasks.json` / provide owner / log 全靠它 | `register` 拒絕（`ESP_ERR_INVALID_ARG`） |
| `kind` | ✅ | `SYSTEM`（房東，不可卸載）／`NORMAL`／`EFFECT`（可由檔案載入） | 拒絕 |
| `level` | 建議 | **宣告與 `on_start` 的次序**；`-1` = 完全唔啟動 | 當 0 |
| `mem[]` / `nmem` | 選 | 靜態記憶體宣告（開機前就知道的量） | 0 項 |
| `declare()` | 選 | 動態記憶體宣告（照**實際硬體**算，例如 ETH 起來才有 TCP 緩衝） | 0 項 |
| `claims[]` / `nclaims` | 選 | 接線獨占宣告（`pin:16` / `bus:spi1`） | 0 條 |
| `provides[]` / `nprovides` | 選 | 對外提供（`BUF` / `JSON` / `U32`） | 0 條 |
| `loops[]` / `nloops` | 選 | 執行單位（0–2 個）；**冇 loop 的任務是合法的**（只做 `on_start`） | 冇 loop |
| `on_start` | 選 | 資源到手後的初始化；**唯一拿到資源指標的地方** | 跳過 |
| `on_stop` | 選 | 停止時的收尾 | 跳過 |

**上限**（超過會被截斷或拒絕，不是無限）：

| 常數 | 值 | 意思 |
|---|---|---|
| `NC_TASK_MAX` | 20 | 管理器最多幾個任務（含 runtime attach 的） |
| `NC_TASK_MEM_MAX` | 8 | 一個任務最多幾項記憶體（`mem[]` + `declare()` **合計**） |
| `NC_TASK_LOOPS_MAX` | 2 | 一個任務最多幾個 loop（例：`nc4` 兩核各一個） |
| `NC_TASK_LAYERS` | 8 | loop 的 layer 只有 0–7 |
| `NC_PLAN_MAX_DECLS` | 64 | 全系統記憶體宣告總數 |
| `NC_PROVIDE_MAX` | 24 | 全系統 provide 總數 |

> **`level` 與 `layer` 是兩個不同的東西**（見 §4）：`level` 在任務上，`layer` 在 loop 上。

---

## 2. 四步

```text
   ① register              ② declare                ③ start                  ④ boot
   ───────────────────────────────────────────────────────────────────────────────────
   nc_task_register()  →  nc_task_declare_all()  →  nc_task_start_all()  →  nc_task_boot()
   只登記、按 level 排序   宣告 → nc_plan           資源到手 → on_start      依 layer 開 loop
                           ＋帳本檢查               → provide → 準備 loop     （0 先）
```

### ① `nc_task_register(&task)` —— 登記

管理器做什麼：

- 檢查 `name` / `kind` 合法、**重名拒絕**（`ESP_ERR_INVALID_STATE`）、滿 20 個拒絕（`ESP_ERR_NO_MEM`）；
- 按 `level` **插入排序**（小的在前；同 level 保持登記次序）；
- 印 `register [name] kind=... level=...`。

你要做什麼：無。**這一步不碰記憶體、不建任何東西。**

### ② `nc_task_declare_all()` —— 宣告

管理器依序做四件事：

1. **`load_cfg()`**：讀 `/data/config/tasks.json`，只取 `level` / `affinity`。
   - `SYSTEM` 任務**跳過**：`affinity = (-1,-1)` = 唔受 config 管，照 C 宣告跑；
   - 租戶：`affinity` 未設定 = `(0,0)`；`level` 未設定 = C 宣告的值。
2. **`append_missing_to_cfg()`**：固件有、config 冇的**租戶** → 補一條
   `{ "name": "...", "level": N, "affinity": [0, 0] }` 到陣列尾端（只加不改）。
3. **`declare_one()`**：把 `mem[]` 和 `declare()` 的結果合併（上限 8 項），逐項
   `nc_plan_declare({service=任務名, name, level, cls, bytes, align})`。
   - 宣告失敗（重名）→ 印 `ESP_LOGE`，這一項不算，**其他項照宣告**。
   - `cfg_level < 0` → **整個任務連宣告都不做**（連記憶體都不留給它）。
4. **`audit()`**：帳本檢查 `claims[]` 的獨占衝突 → 後者不算（⚠️ 有 bug，見 §7）。

### ③ `nc_task_start_all()` —— 資源到手 → `on_start`

對每個 slot（按 level 次序）：

1. **`gather_resources()`**：逐項 `nc_plan_get(name)`。
   **任何一項拿不到 → 整個任務不啟動**（回 `ESP_ERR_NO_MEM`），`on_start` 不會被呼叫。
2. **`on_start(view, n)`**：把 n 個 `{ptr, len}` 交給你（順序 = `mem[]` 先、`declare()` 後）。
   回非 `ESP_OK` → 任務不啟動（已拿到的記憶體**不會退回**，見 §5）。
3. **`prepare_loops()`**：把 `loops[]` 抄進 runtime 結構，並**套用 affinity**：
   - `(0,0)` → `enabled = false`（loop 唔開，但 `on_start` 照跑）；
   - `(1,1)` → 印警告、照宣告的 core；
   - 宣告的核唔准（例如宣告 core 0 但 affinity 只准 core 1）→ **搬去准的那一核**。
4. **`prov_register()`**：provide 進全局表（滿 24 條就註冊不到）。

**這一步不開 loop。** 這是刻意的：`on_start` 必須在「所有任務的資源都配好」之後、
「任何 loop 開始跑」之前完成。

### ④ `nc_task_boot()` —— 依 layer 開 loop

- 外層 `layer = 0..7`，內層掃所有任務的所有 loop；
- 只開「`started` 且 `enabled` 且 `layer` 相符」的 loop；
- `core = -1` → `xTaskCreate`（不指定核）；`0/1` → `xTaskCreatePinnedToCore`；
- 可重複呼叫（已開的跳過），所以「runtime 加一個 loop 再 boot 一次」是安全的。

---

## 3. 記憶體：管理器憑什麼決定要多少

三句話：**你宣告、它加總、每個 class 只向系統要一次。**

```text
你的 declare()/mem[]
   │  nc_plan_declare(service, name, level, cls, bytes, align)
   ▼
plan 的宣告表（全域 64 項，按 level 排序）
   │  nc_plan_commit(avail[3], allow_shed)
   ▼
每個 class：declared[] 加總 → 同 avail[class] 比 → 逐項成功／失敗
   │  成功項的 bytes 合計 = planned[class]
   ▼
每個 class 一次 carve（不是逐項 malloc）→ 切給各項 → nc_plan_get(name) 取指標
```

| class | 什麼時候用 |
|---|---|
| `NC_PLAN_DMA` | 要給 DMA 的緩衝（必須 internal + DMA-capable） |
| `NC_PLAN_INTERNAL` | 一般內部 RAM（最快） |
| `NC_PLAN_PSRAM` | 大塊、不常存取（arena、framebuffer） |

**`avail[]` 是呼叫端給的**（`main.c` 用 `heap_caps_get_largest_free_block()` 量當下最大可用塊）。
所以「夠不夠」不是寫死的數字，是**開機當下量出來的**。

**失敗是逐項的，不是整批**：某一項塞不下 → 該項 `nc_plan_get()` 回 `NULL`，
其他項照配。但對任務而言「有一項拿不到」= 整個任務不啟動（§2 ③ 第 1 步）。

**宣告窗口在 `commit` 時關閉**：commit 之後 `nc_plan_declare()` 回 `ESP_ERR_INVALID_STATE`。
這就是為什麼 **runtime attach 的任務不可以帶 `mem[]`**（`nc_task_attach` 會直接拒絕）——
它們的記憶體要從父層 arena 切（FXE 引擎的 arena）。

---

## 4. 次序：`level` 與 `layer`

| | `level`（任務欄位） | `layer`（loop 欄位） |
|---|---|---|
| 管什麼 | **宣告**與 **`on_start`** 的次序 | **loop 啟動**的次序 |
| 範圍 | 0 以上；`-1` = 唔啟動 | 0–7（`NC_TASK_LAYERS`） |
| 誰用 | `declare_all` / `start_all` | `nc_task_boot` |

**兩者互相獨立**：`nc_task_boot()` 只看 `rt->def->layer`，**完全不理任務的 `level`**。
所以「任務 level 1、它的 loop layer 3」是完全正常的（`fx` 就是這樣）。

### 實際保證到哪裡

```text
所有任務的 on_start（不分 level，全部）   ← 單執行緒、依 level 次序做完
        ↓
layer 0 的 loop 全部建立
        ↓        ← 冇等它們「初始化完成」
layer 1 的 loop 全部建立
        ↓
…
```

- ✅ **`on_start` 有 barrier**：全部做完才開第一個 loop。
- ❌ **loop 之間冇 barrier**：layer 0 的 loop 剛 `xTaskCreate` 完，layer 1 就接著開；
  管理器**不知道** layer 0 的 loop 是否已經初始化好。

> **同上游一致（已確認）**：舊專案（mp_Net-Core）的 `task_manager.py` 是逐層問
> 「這一層所有任務都 `on_start` 完了嗎」，完了才 `_boot_layer += 1`；
> **全部 `on_start` 完成之後，才進入 loop**。它**不會**「一層 `on_start` 完就即刻開該層的 loop」。
>
> 我們的做法是一次過把**所有**任務的 `on_start` 依 `level` 次序做完，然後才開第一個 loop ——
> **結果一樣，而且少一層狀態**（唔需要 `_boot_layer` 游標去問「做完未」）。
> 所以「`level` 不是 barrier」**不是缺口**：兩邊都只保證 `on_start` 的 barrier，
> 兩邊都唔保證「loop 已初始化完成」。
>
> 如果將來真的要後者（例如 fx 的 loop 一開頭就要用上一層的 provide），
> 才需要 `nc_task_ready()`（見 §7 缺口 1）。今日**冇需要**。

### 與上游 `task_manager.py` 的實際差異

上游是**每核一個 runner 迴圈**，逐層放行：

```python
# task_manager.py（簡化）
def _update_tasks(self, core_id):        # 每核每輪呼叫
    for name, affinity in self.config.items():
        if affinity[core_id] == 1 and self._task_eligible_for_boot(name):
            task.on_start()              # ← 這一層的任務在這裡初始化
    if not self._boot_done:
        self._check_boot_layer_done()    # ← 該層全部進了 active 才 _boot_layer += 1
```

| | 上游（mp_Net-Core） | 我們（C） |
|---|---|---|
| 次序由誰決定 | `layer`（一個任務一個） | `level`（on_start）+ `layer`（loop）**兩套** |
| 啟動方式 | 每核 runner 逐層放行 | 單執行緒：全部 `on_start` 做完，才開 loop |
| 層與層之間 | 用 `_boot_layer` 游標逐層問「這一層 `on_start` 做完未」，做完才進下一層；**全部做完才開 loop** | 一次過做完**所有** `on_start`（依 `level` 次序），然後才開 loop —— **結果相同**，但少一個游標 |
| 非同步 init | 特例：`fs_scan` 要等 `fs_scan_done` | 冇機制（要加 ready） |
| 某個任務啟動失敗 | 它不進 active → 該層**永遠等不到** → 卡住（靠 `advance_to_running()` 強制放行） | 該任務不啟動，**其他照跑**，boot 繼續 |

**兩邊的分別只有一處，而且不是 barrier**：上游用「游標 ＋ 問做完未」來保證 `on_start` 的次序，
我們直接一次過順序做完（單執行緒，次序天然成立）。上游那個設計的副作用是
**一個任務失敗就卡死整個開機**（所以他們要留 `advance_to_running()` 逃生門）；
我們的設計是「失敗就跳過，開機一定要完成」。

> 真正兩邊都**冇**保證的是「上一層的 loop 已經初始化完成」。要那個保證才需要
> `nc_task_ready()`（§7 缺口 1）—— 今日用不到。


---

## 5. 關閉：順序、等待、歸還

```text
nc_task_stop_all()  /  nc_task_detach(name)
        │
        ├─ 由尾向前（level 大的先停）
        ▼
   shutdown_slot(slot)
        ├─ ① stop_loops(1000 ms)：逐個 loop 設 exit_req，等它自己返回
        │      └─ 超時 → 回 false → **整個 shutdown 中止**（唔強殺、唔卸載）
        ├─ ② on_stop()：你的收尾
        ├─ ③ prov_unregister()：provide 從全局表移除
        ├─ ④ started = false、清空 view
        └─ ⑤ （只有 detach）從 slot 表移除（memmove 補位）
```

**loop 一定要看結束旗標**，否則管理器停不下來（回 `ESP_ERR_TIMEOUT`，且**不會** `vTaskDelete` 強殺）：

```c
static void my_loop(void *arg)
{
    while (!nc_task_exit_requested()) {
        /* ... */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
```

### 歸還表

| 東西 | 誰歸還 | 什麼時候 |
|---|---|---|
| loop 的 stack / TCB | FreeRTOS（`vTaskDelete(NULL)`） | loop 函式返回時 |
| provide 條目 | 管理器（`prov_unregister`） | `shutdown_slot` |
| slot（任務格） | 管理器 | `detach` 時 |
| `claims[]` | 冇 runtime 資源，只是帳 | — |
| **plan 的記憶體** | **冇人歸還** | `commit` 時一次 carve，永久屬於該 class |
| **FXE 模組的記憶體** | FXE 引擎（arena 回收） | `nc_fx_unload()` |

**「停止」≠「釋放記憶體」。** plan 沒有 release：開機 carve 一次，之後只有「誰拿到」，
沒有「還回去」。所以：

- 停止一個任務 → 它的 loop 消失、provide 消失、`on_start` 拿到的指標**仍然有效**（但你不該再用）；
- 重新啟動同一個任務 → `gather_resources()` 會拿到**同一批指標**（不是新配的）。

這是刻意的：嵌入式不需要 heap 碎片，也不需要 runtime 重新規劃記憶體。
runtime 要增減的東西（`.fxe` 模組）走 arena，不進 plan。

---

## 6. 狀態機與可觀察的 log

```text
         register            declare              start                boot
  ──► [已登記] ──────────► [已宣告] ──────────► [started=1] ────────► [running]
       slot.used=1          plan 有記錄          on_start 完成          loop 在跑
                                                  provide 已註冊
        │                     │                     │                    │
        │                     │  cfg_level < 0      │  affinity (0,0)    │
        │                     └──► 唔宣告、唔啟動    └──► started=1       │
        │                                               running=0        │
        └───────────────────────── stop / detach ◄──────────────────────┘
                                    on_stop → 收回 provide → started=0
```

| 你會在 log 看到 | 意思 |
|---|---|
| `register [x] kind=normal level=3` | ① 完成 |
| `cfg x task level= 3 affinity=(0,0)` | ② 讀到 config 的結果 |
| `cfg 補寫 x` | ② 補了一條進 `tasks.json` |
| `start [x] kind=normal mem=1 loops=1 prov=0` | ③ 完成（`on_start` 已跑） |
| `loop [x] L3 → core 1` | ④ 這個 loop 真的在 core 1 跑起來 |
| `loop [x] 結束（N 輪）` | loop 收到結束旗標並返回 |
| `detach [x]` | runtime 任務被卸載 |

**三種「看起來沒跑」的分別**（很容易搞混）：

| 現象 | `started` | `running` | 原因 |
|---|---|---|---|
| `level: -1` | 0 | 0 | 完全唔啟動（連記憶體都唔宣告） |
| `affinity: [0,0]` | 1 | 0 | `on_start` 照跑，loop 唔開 |
| 冇 loop 的任務（例如 `net`） | 1 | 0 | 合法：它只提供記憶體，不需要跑 |

---

## 7. 缺口與已知 bug

| # | 問題 | 影響 | 建議 |
|---|---|---|---|
| 1 | **loop 之間冇 ready barrier** | layer N 的 loop 未初始化完，layer N+1 已開；依賴「上一層已就緒」的任務會踩空 | loop 增加 `nc_task_ready()`：初始化完呼叫一次；`boot` 在每層之後等全部 ready（有 timeout） |
| 2 | **plan 冇 release** | 停止／重啟不會換記憶體；runtime 任務不能帶 `mem[]` | 保持現狀（刻意），但文件要寫明；runtime 一律走 arena |
| 3 | **`level` 與 `layer` 兩套次序** | 同一個任務有兩個「先後」，容易填錯 | 文件寫明分工（§4）；長期考慮讓 `layer` 預設 = `level` |
| 4 | **`audit()` 的 claims 迴圈有 bug** | `s_slots[i].nclaims--` 之後迴圈條件跟著變小 → **跳過剩下的 claim**，而且被「刪掉」的是最後一條，不是衝突那條 | 改成「標記該條失效」而不是減 `nclaims` |
| 5 | `on_start` 失敗後記憶體不退回 | 該任務的記憶體永遠佔著 | 刻意（plan 無 release），但要在 log 講清楚 |
| 6 | `nc_task_attach()` 失敗會自動 detach 回滾 | 正常；但若 `on_start` 已註冊了 provide，回滾會一併收回 | 保持 |

---

## 8. 檢查清單（寫新任務前）

1. `name` 唯一、`kind` 選對（房東 = `SYSTEM`，租戶 = `NORMAL`/`EFFECT`）。
2. 記憶體：能用 `declare()` 就唔好用寫死的 `mem[]`（照實際硬體算）。
   每一項都要有**全系統唯一**的名字（`<域>.<用途>`，例如 `nc4.rx`）。
3. 需要 loop 才寫 `loops[]`；只做初始化就唔要 loop。
4. `on_start` 是**唯一**拿資源指標的地方；不要在別處 `nc_plan_get()`。
5. loop 一定要 `while (!nc_task_exit_requested())`，並且**有阻塞**（`vTaskDelay` / queue / semaphore）。
6. 要給別人用的東西 → `provides[]`；要獨占的接線 → `claims[]`。
7. 停止時要收的東西 → `on_stop()`；**記憶體不用收**（plan 不還）。
