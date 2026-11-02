# 命名慣例（C 版）

> **用途**：看到符號或檔名，馬上知道它屬於哪一層、哪個子系統、是任務還是動作。
> **狀態**：2026-11-02 統一（舊的 `nc_tm_*` / `nc_res_*` / `ota_register` / `store_*` / `ws_nc4_*` 已全部改掉）。

---

## 0. 一句話

**`nc_<子系統>_<角色>`；同一個字同時有任務與動作時，用 `<域>_task` / `<域>_action` 後綴區分。**

---

## 1. 為什麼是後綴：`pixel_task` vs `pixel_action`

同一個「域」可以同時有兩種東西，而且未來一定會遇到（`pixel` 就是）：

| 東西 | 是什麼 | 名字 |
|---|---|---|
| **任務** | 一條 loop：收發、渲染、節拍；佔核心、佔 stack | `pixel_task` |
| **動作** | 一組指令：cmd → handler；跑在解碼核上 | `pixel_action` |

```text
components/tasks/pixel_task/
    nc_pixel_task.c            const nc_task_t nc_pixel_task;      ← 給 nc_task_register()
components/action/pixel_action/
    nc_pixel_action.c          nc_pixel_action_register(dec);      ← 開機註冊指令
```

**後綴而不是前綴**：`pixel` 是「做什麼」，`task` / `action` 是「它在系統裡的角色」；
先讀域、再讀角色，跟口語一致（「pixel 的 task」「pixel 的 action」）。
前綴會變成 `nc_task_pixel` / `nc_action_pixel`，兩個不同層的東西擠在同一個字首後面，
掃檔案時反而難分。

> 目前的 `nc4` / `fx` / `store` 還沒有這個問題（沒有同名域），所以維持
> `nc_task`、`nc_fx`、`nc_store`；一旦出現同名域，就照上面的後綴規則拆。

---

## 2. 前綴表（子系統）

| 層 | 子系統 | 前綴 | 檔案 |
|---|---|---|---|
| 協議 | NC4 | `nc4_` | `nc4.c` / `nc4_bus.c` |
| 記憶體 | plan / hub / 原語 | `nc_plan_` / `nc_hub_` / `nc_mem_` | 同名 `.c` |
| 任務 | 任務管理器 | `nc_task_` | `nc_task.c`（政策）、`nc_task_loop.c`（執行層）、`nc_task_priv.h` |
| FAT | store | `nc_store_` | `nc_store.c` |
| 通道 | WS / UDP / HTTP | `nc4_ws_` / `nc4_udp_` / `nc4_http_` | `nc4_ws.c` … |
| 動作 | 域 | `nc_<域>_action_` | `nc_ota_action.c` / `nc_file_action.c` |
| 效果 | fx | `nc_fx_` | `nc_fx.c` / `nc_fx_manifest.c` |

## 3. 四條規則

1. **不要兩套前綴。** 以前任務層同時有 `nc_tm_*` 與 `nc_task_*`，現在全部 `nc_task_*`。
2. **檔名 = 前綴 + 角色。** `nc_task.c` 是政策、`nc_task_loop.c` 是執行層、
   `nc_ota_action.c` 是 OTA 的動作；看到檔名就知道它負責哪一段。
3. **型別 = `nc_<子系統>_t`；列舉值 = `NC_<子系統>_...`。**
   例：`nc_task_kind_t` / `NC_TASK_SYSTEM`；`nc_store_mode_t` / `NC_STORE_MODE_APP`。
4. **動作只有一個入口。** 一個域一個元件，對外只有
   `void nc_<域>_action_register(nc4_decoder_t *decoder)`；handler 命名 `on_<動詞>`。

---

## 4. 新舊對照（查舊 commit / 舊文件用）

| 舊 | 新 |
|---|---|
| `nc_tm_reset` / `nc_tm_json` / `nc_tm_boot` … | `nc_task_reset` / `nc_task_json` / `nc_task_boot` … |
| `nc_res_req_t` / `nc_res_view_t` | `nc_task_res_t` / `nc_task_res_view_t` |
| `nc_loop_rt_*`（內部） | `nc_task_rt_*` |
| `ota_register()` / `ota_cmds.c` | `nc_ota_action_register()` / `nc_ota_action.c` |
| `file_cmds_register()` / `file_cmds.c` | `nc_file_action_register()` / `nc_file_action.c` |
| `nc_action_ota_*`（上一版） | `nc_ota_action_*`（後綴版） |
| `store_init` / `store_mode` … | `nc_store_init` / `nc_store_mode` … |
| `ws_nc4_start` / `udp_nc4_start` / `http_cfg_register` | `nc4_ws_start` / `nc4_udp_start` / `nc4_http_register` |
| `nc_tm.c`（管理器） | `nc_task.c` |
| `nc_task.c`（舊執行層） | `nc_task_loop.c` |
