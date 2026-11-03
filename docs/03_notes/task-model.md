# 任務模型：房東與租戶

> **用途**：講清楚「核心」與「任務」的分界、開機時 tasks.json 怎麼被讀／補寫、
> `.fxe` 怎麼變成任務。這份是**現況**（已上板驗證），不是提案。
>
> 相關：[task-fx-open-questions.md](task-fx-open-questions.md)（未定問題）、
> [routing.md](routing.md)（解碼路由）、[writing-a-task.md](writing-a-task.md)（怎麼寫任務）

---

## 1. 兩層：核心（房東）與任務（租戶）

| | 核心 | 任務 |
|---|---|---|
| 是什麼 | `nc4`（解碼）、`net`（傳輸緩衝）、`core`（慢工作）、`fx`（載入器） | 一般任務、`.fxe` 載入的任務 |
| `kind` | `NC_TASK_SYSTEM` | `NC_TASK_NORMAL` / `NC_TASK_EFFECT` |
| config 管不管 | **不管**：照 C 宣告跑，唔會寫入 `tasks.json` | 管 `level` / `affinity` |
| 跑幾核 | 照宣告（`nc4` 天生兩核各一個 loop） | 一個任務一個核，`(1,1)` 禁止 |

**為什麼要分**：核心要保證「就算用戶一個 config 都冇，板子仍然開得機、收得到指令」。
所以 config 只能管租戶，管不到房東。

---

## 2. 三個 component

```
components/sys/task/          型別 nc_task_t + loop 執行層（建立 / 量測 / 停止）
components/sys/task_manager/  管理器：登記 → 宣告 → 啟動 → 開 loop → 狀態
components/app/tasks/<name>/  每個任務一個 component（只放宣告與 loop 本體）
```

對照舊專案：`lib/sys/task.py` / `lib/sys/task_manager.py` / `slave/tasks/*.py`。
任務**不碰** `xTaskCreate`、不碰 registry —— 只回傳 `const nc_task_t *`。

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
| `affinity` | `[core0, core1]`，只可以有一個 `1`；`[0,0]` = 唔跑 |

**補寫規則**（用戶冇機會知道嘅嘢才補）：固件或 `.fxe` 有、`tasks.json` 冇 →
開機時加在陣列尾端，**保留原本排版**，永不覆蓋用戶寫過的值。

---

## 5. `.fxe` 變成任務

```
/data/pixel/effects/*.fxe   kind = effect
/data/tasks/*.fxe           kind = normal（任務）
```

`.fxe` 自帶：檔頭（wasm / aot + target）、manifest（kind / core / layer / prio /
fps / px / mem / budget / 指令）。掃到之後 `nc_fx_load()` → 驗證 → 從 arena 切記憶體 →
`nc_task_attach()` 登記成 runtime 任務。

> ⚠️ **執行引擎（WAMR）未接**：現階段 `.fxe` 只做到「掃到、驗證、登記」，
> `loops = []`，payload 未會跑。

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
3. `.fxe` 任務的 `level` 預設應由 manifest 的 `layer` 決定（現在是 0）
4. `system.json` 的 `routes` 補寫（設計已定，未實作）
