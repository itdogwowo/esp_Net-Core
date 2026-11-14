# 任務模型：房東與租戶

> **用途**：講清楚「核心」與「任務」的分界、開機時 tasks.json 怎麼被讀／補寫、
> `.fxe` 怎麼變成任務。這份是**現況**（已上板驗證），不是提案。
>
> 相關：[task-lifecycle.md](task-lifecycle.md)（建立／啟動／停止的完整流程與歸還規則）、
> [task-fx-open-questions.md](task-fx-open-questions.md)（未定問題）、
> [system-model.md](system-model.md)（在上層：System 組件與核心組——任務是組的一部分）、
> [routing.md](routing.md)（解碼路由）、[writing-a-task.md](writing-a-task.md)（怎麼寫任務）

---

## 1. 兩層：核心（房東）與任務（租戶）

| | 核心 | 任務 |
|---|---|---|
| 是什麼 | `nc4`（解碼）、`net`（傳輸緩衝）、`core`（慢工作）、**FXE 引擎**（載入器） | 一般任務、`.fxe` 載入的模組 |
| `kind` | `NC_TASK_SYSTEM` | `NC_TASK_NORMAL` / `NC_TASK_EFFECT` |
| config 管不管 | **不管**：照 C 宣告跑，唔會寫入 `tasks.json` | 管 `level` / `affinity` |
| 跑幾核 | 照宣告（`nc4` 天生兩核各一個 loop） | 一個任務一個核，`(1,1)` 禁止 |

**為什麼要分**：核心要保證「就算用戶一個 config 都冇，板子仍然開得機、收得到指令」。
所以 config 只能管租戶，管不到房東。

> **名字對齊上游**：`net` 上游叫 `net_bus`（`slave/lib/sys/net_bus.py`，class `NetBus`，
> 「純傳輸層」）；`core` 上游冇對應（自創）；`fx` 應拆成 FXE 引擎 + pixel 渲染器。
> 完整對照與建議見 [naming.md](naming.md) §6。

---

## 2. 兩個角色：模型（model）與管理器（manager）

| 目錄 | 對應上游 | 角色 | 裡面有什麼 |
|---|---|---|---|
| `sys/task/` | `lib/sys/task.py`（`class Task`） | **模型** | `nc_task_t` 型別、五個生命週期階段的契約、loop 執行層（`xTaskCreate` / 量測 / 停止） |
| `sys/task_manager/` | `lib/sys/task_manager.py` | **管理器** | 登記 → 宣告 → 啟動 → 開 loop → 停止 → 卸載，全部任務的次序與記憶體 |
| `app/tasks/<name>/` | `slave/tasks/*.py` | **一個任務** | 只放宣告與 loop 本體，回傳 `const nc_task_t *` |

**每個任務都是「模型的實例」**：不管它是核心、租戶、還是 `.fxe` 載入的模組，
都必須走同一組五個階段 —— 這就是「其他所有子任務都模仿 `task`」的意思：

| 階段 | 上游 `Task` | 我們（C） | 誰呼叫 |
|---|---|---|---|
| ① **宣告** | class 屬性 `log_schema` / `hw`；`register_task(layer, affinity)` | `mem[]` / `declare()` / `claims[]` / `provides[]` / `level` / `kind` | 管理器（`declare_all`） |
| ② **啟動** | `on_start()` | `on_start(res, n)` | 管理器（`start_all`） |
| ③ **迴圈** | `loop()` | `loops[].fn(arg)` | 管理器（`boot`）+ FreeRTOS |
| ④ **停止** | `on_stop()` | `on_stop()` | 管理器（`shutdown_slot`） |
| ⑤ **卸載** | 管理器移除實例 | `nc_task_detach()`（**只有 runtime 任務**） | 管理器 |

差別只有一處：上游的 `Task` 是 Python class（有方法）；我們的 `nc_task_t` 是**純資料 + 函式指標**，
五個階段由管理器驅動。**語義一樣，實作方式不同**（C 沒有繼承，就把「子類」變成「一個 struct 實例」）。

> **「記憶體」＝ 緩衝區**：`mem[]` / `declare()` 宣告的每一項都是一條 `nc_buf`。
> 誰建立、幾時拿到、停止之後還在不在 —— 契約 C1–C11 在
> [buffers.md §9](buffers.md)，怎麼搭配在 [buffer-patterns.md](buffer-patterns.md)。

任務**不碰** `xTaskCreate`、不碰 registry —— 只回傳一個 `const nc_task_t *`。


---

## 3. 開機流程

```
① 格式化 data（第一次）+ 寫入基礎 data（system / network / tasks）
② 登記核心任務（nc4 / net / core / fx）
③ nc_task_declare_all()
     ├─ 讀 tasks.json
     ├─ 核心任務：跳過（config 管不到）
     ├─ 租戶：cfg 有 → level / affinity 用 cfg；冇 → 用 C 預設
     ├─ level = -1 → 唔 declare、唔啟動
     └─ 冇寫的租戶 → 自動補寫入 tasks.json（只加不改）
④ 資源 commit → start_all → boot（按 layer 開 loop）
⑤ 掃 .fxe：/data/pixel/effects/（效果）+ /data/tasks/（任務）
     └─ 掃到的 attach 成任務 → nc_task_sync_cfg() 再補寫一次
```

---

## 4. `tasks.json`

只列**租戶**。核心任務永遠不出現在這裡。

```jsonc
{
  "version": 1,
  "tasks": [
    { "name": "hello", "level": 6, "affinity": [1, 0] }
  ]
}
```

| 欄位 | 意思 |
|---|---|
| `name` | 任務名（固件宣告或 `.fxe` manifest 的名字） |
| `level` | `-1` = 唔啟動；同一 level 內按**註冊次序** |
| `affinity` | `[core0, core1]`；`[0,0]` = 唔跑；只可以有一個 `1` |

**未設定 = `[0,0]`**：租戶任務冇寫 `affinity`（或者成條條目都冇）就當 `(0,0)` →
`on_start` 照跑、`loops` 唔開（`started = 1`、`running = 0`）。
核心（`kind = system`）唔受影響，照 C 宣告跑。
`null` 或缺格一樣當 `0`（唔係 `1`）：`[null, 1]` = 只准 core 1。

**補寫規則**（用戶冇機會知道嘅嘢才補）：固件或 `.fxe` 有、`tasks.json` 冇 →
開機時加在陣列尾端，**保留原本排版**，永不覆蓋用戶寫過的值。
補進去的條目一律寫 `[0,0]`（預設關閉）：與「未設定 = `[0,0]`」一致，
所以補寫前後行為一樣，唔會出現「重開機先跑」。要跑就自己改成 `[1,0]` 或 `[0,1]`。

---

## 5. `.fxe` 變成任務（FXE 引擎）

```
/data/pixel/effects/*.fxe   kind = effect
/data/tasks/*.fxe           kind = normal（任務）
```

`.fxe` 自帶：檔頭（wasm / aot + target）、manifest（kind / core / layer / prio /
fps / px / mem / budget / 指令）。掃到之後 **FXE 引擎**：`nc_fx_load()` → 驗證 →
從 arena 切記憶體 → `nc_task_attach()` 登記成 runtime 任務。

> ⚠️ **執行引擎（WAMR）未接**：現階段 `.fxe` 只做到「掃到、驗證、登記」，
> `loops = []`，payload 未會跑。

**FXE 引擎的兩個限制（現況）**：

| 限制 | 意思 | 影響 |
|---|---|---|
| 同時只有 **一個 live 模組** | `s_live` 是單一 static 指標；`nc_fx_select()` 換場，舊的兩幀後 retire | 不能兩個模組同時輸出 |
| render loop **寫死 core 1** | `NC_FX_TASK_CORE 1`（`main.c`）、`nc_fx_set_task_placement(1, 3)`；manifest 宣告別的核會被拒（`core = -1` 除外） | 模組不能選核；要跑兩核得先拆成「每核一個 renderer 實例」 |

> 名詞：**FXE 引擎**（載入器，房東）≠ **pixel 渲染器**（把 framebuffer 送出去，租戶）。
> 兩者現在擠在同一個 `fx` 任務裡，見 [naming.md](naming.md) §5。

---

## 6. 已驗證（S3，2026-11-03，全程 NC4 OTA）

| 測什麼 | 結果 |
|---|---|
| `hello` 任務 | 每秒印一次，`dt = 1,000,000 µs ±1` |
| `level: -1` | `started = 0`、`loops = []`（完全唔啟動） |
| `affinity: [0,1]` | 由 core 0 搬到 core 1 執行 |
| 冇 `tasks.json` | 開機自動寫回基礎檔（604 B），核心照跑 |
| 固件有、config 冇 | `probe` 自動補寫入 config |
| `.fxe` 掃描 | `demo.fxe` → 任務 `demo`（`runtime=1`），並補入 config |

---

## 7. 未做

1. WAMR 執行引擎（`.fxe` 目前只登記不執行）
2. manifest 加 `name`（現在名字仍來自檔名）
3. `.fxe` 任務的 `level` 預設應由 manifest 的 `layer` 決定（現在寫死 `5`，`layer` 只驗不用）
4. `system.json` 的 `routes` 補寫（設計已定，未實作）
