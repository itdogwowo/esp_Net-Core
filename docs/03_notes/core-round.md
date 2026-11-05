# 每核「大循環」量測（core round）

> **一句話**：同一個核心上面的所有 loop 各自跑完一次 = 一輪（round）。
> 量的是這一輪的 **period（wall time）**、**exec（所有 fn 時間總和）**、
> **cycles（CPU cycles）**、**jitter（波幅）** 與 **duty（忙碌比例）**。
>
> 實作：`firmware/components/sys/epoch/`（純邏輯）＋ `nc_task_loop.c` 的 trampoline。

---

## 0. 為什麼這樣定義

目前每個 loop 都是**獨立的 FreeRTOS task**（`nc_task_rt_start` → `xTaskCreate`），
沒有一個中央排程器會「順序跑完所有任務」。所以「大循環」只能用可量測的等價條件界定：

> 這一核的每一條 loop，都至少跑過一次 fn（期間沒有再發生）→ 一輪完成。

這不是完美的「一個 loop 跑完全部任務」，但它有三個好處：
1. 不需要改執行模型（不會為了量測而把 task 改成 coroutine）；
2. 任何一條 loop 加／減都自動反映（token 增減）；
3. 量測成本 O(1)，每輪一次 mark。

---

## 1. 名詞

| 欄位 | 意思 | 備註 |
|---|---|---|
| `period` | 上一輪完成 → 這一輪完成的 wall time | **由最慢的 loop 決定**（例：hello 1 s → round ≈ 1 s） |
| `exec` | 這一輪所有 loop 的 `fn` 執行時間**總和** | 同一條 loop 跑多次都計入 |
| `cycles` | 同一輪的 CPU cycle 差 | 同 `exec` 對照可看出被搶佔／量測誤差 |
| `jitter` | `period_max − period_min` | 波幅 |
| `duty` | `exec_avg / period_avg × 1000`（‰） | 這個核有多忙 |
| `rounds` | 完成過幾多輪 | 第一輪只做 base，不計 period |

---

## 2. 怎麼讀

```
STATUS_GET  keys=epoch     →     0x1102 STATUS_RSP
```

```json
{"core0":{"rounds":120,"loops":3,
          "period":{"last":10004,"avg":10002,"min":9980,"max":10070,"jitter":90},
          "exec":{"last":1180,"avg":1150,"max":1900},
          "cycles":{"last":414000,"avg":400000},
          "duty":114},
 "core1":{...}}
```

- `duty = 114‰` → 這個核約 11.4% 的時間在跑任務。
- `cycles/exec` 比值 ≈ CPU 頻率 → 正常；比值偏低代表 `exec` 內有等 I/O／被搶佔。
- `jitter` 突然變大 → 有異常常駐工作或 flash 操作。

---

## 3. 誤差來源（要信之前先看這些）

| 來源 | 影響 | 怎麼看 |
|---|---|---|
| `esp_timer` 解析度 | 1 µs | 對 ms 級的輪次可忽略 |
| 每次 mark 的成本 | 約 1–2 µs（含一次 cycle 讀取） | 看 `nc_task_json` 的 `wrap_avg_us`（已包含它） |
| FreeRTOS tick | HZ=1000 → 1 ms | `period_ms` 太小（< 1 tick）會被量化 |
| 高優先序任務 | `exec` 是 wall time，不是純 CPU | 用 `cycles` 對照 |
| 其他核／快取／PSRAM | 抖動、`exec` 拉長 | 看 `jitter` 與 `exec.max` |
| 第一輪 | 只做 base | `rounds` 從第 2 次完成才 +1 |

> **判斷原則**：如果 `period` 的 jitter < 5%、`exec` 的 max 不超過 avg 的 2 倍、
> 而且 `cycles/exec` 穩定，這個數字就可以信。反過來就不要拿它做結論。

---

## 4. 用 bench 任務驗證（線性 × 波幅 × 影響）

`bench_a` / `bench_b` / `bench_c` 三條任務（`components/tasks/test_bench_tasks/`）：

- 每 **5 ms** 燒 **100,000 CPU cycles**（≈ **278 µs** @360 MHz、250 µs @400 MHz）。
- 預設 **不跑**（tasks.json 補寫成 `[0,0]`）。
- 開啟方式：改 `/data/config/tasks.json` 的 `affinity`，例如
  `{ "name": "bench_a", "level": 4, "affinity": [0, 1] }` → 跑 core1；重開機。

### 4.1 預期結果（以 1 秒的 round 為例）

| 開啟 | 每輪跑幾次 | 每輪 exec 增加 | duty 增加 |
|---|---:|---:|---:|
| 0 條 | — | 0 | 0 |
| 1 條 | 200 | ≈ 55.6 ms | ≈ +5.6% |
| 2 條 | 400 | ≈ 111 ms | ≈ +11.1% |
| 3 條 | 600 | ≈ 167 ms | ≈ +16.7% |

- **線性**：1→2→3 條，`exec_avg` 應該接近等差。
- **波幅**：`jitter` 應該維持在 tick 級（1–2 ms）；如果開啟 bench 後 jitter 明顯變大，
  代表量測被搶佔影響，要檢查優先序。
- **對效能影響**：同時看 `nc_task_json` 裡面其他 loop 的 `avg_us` / `max_us`，
  以及 `epoch.exec` 的差值 —— 兩者應該吻合（bench 的成本 ≈ 其他任務延遲的增加）。

### 4.2 驗證步驟

1. 燒錄，開機後 `STATUS_GET keys=epoch`（等 10 秒讓 rounds ≥ 10）。
2. 記下 core1 的 `period.avg`、`exec.avg`、`jitter`。
3. 改 `tasks.json` 開 `bench_a`，重啟，再讀一次。
4. 重複開 `bench_b`、`bench_c`。
5. 比較：`exec` 是否每次 +≈55 ms？`jitter` 是否穩定？其他任務的 `avg_us` 有沒有變？

> **注意**：如果同一核有 1 秒的 loop（hello），round 就是 1 秒；
> 想要 5–10 ms 的 round，先把 hello 的 affinity 設成 `[0,0]`（或改 period）。

---

## 5. 限制與坑

| 情況 | 行為 |
|---|---|
| 同核有一條很慢的 loop（hello 1 s） | round 會被它拖到 1 s —— 這是定義，不是 bug |
| 停用（`nc_task_enable(false)`）的 loop | **仍然參與**（exec=0），否則其他 loop 的 round 會永遠等它 |
| `run_once` 的 loop | 跑完退出時移除 token；不會卡住 |
| 同一 loop 一輪跑多次 | 只算一次參與者，但 exec 照加（真實 CPU 用量） |
| 診斷／低頻 loop（例如 monitor 1 秒一次） | 要設 `.no_epoch = true`，否則該核的 round 會被拉到它的 period |
| 每核 > 8 條 loop | `nc_epoch_add` 回 -1（token 用完）；目前最多 4–5 條 |
| round > 10.7 s @400 MHz | `cycles` 的 u32 差不夠用；`period` 仍然正確 |

---

## 6. 主機測試

```bash
bash tools/hosttest/epoch_test/run.sh     # 10 個 case
bash tools/hosttest/run_all.sh            # 8 支全跑
```

測的是「一輪怎樣才算完成」與統計數學（per-loop 重跑、中途加減 loop、
token 上限、cycles wrap、JSON 合法），不需要板子。

---

## 7. 目前成本

| 板子 | DIRAM 增加 | 備註 |
|---|---:|---|
| S3 | **+832 B** | epoch 表 ＋ bench 三條任務 ＋ monitor／0x1005 的程式碼 |
| P4 | **+840 B** | 同上 |

- 基準：epoch 之前 S3 97,929 / P4 129,023；現在 98,761 / 129,863。
- bench 任務預設不跑，執行期成本 0；monitor 自己 3,072 B 棧（`no_epoch`）。
- epoch 每個 loop 每次迭代約 1–2 µs（已算在 `wrap_us`）。
