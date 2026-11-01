# 緩衝管理器 — 實作現況

> **用途**：設計（[buffer-manager.md](buffer-manager.md)）與程式碼之間的對照：做了什麼、沒做什麼、量到什麼。
> **狀態**：交換管理器 v2 完成（服務宣告 / 每 class 一次 carve / 逐項失敗 / hub 由 plan 供記憶體）；
> global 狀態與 WiFi reserve 尚未實作。
> **相關**：[memory.md](memory.md)（原語與壽命）、[testing.md](../02_guides/testing.md)（量測方法）

---

## 1. 範圍

```
交換管理器（已實作）          global 狀態（未實作，將是靜態服務）
  · 宣告 → 一次 carve            · log ring
  · slot 轉手（hub）             · 每服務一格 status
  · 不等待、不丟資料             · 不進 plan
```

管理器**不認得應用**：它只收到 `{ service, name, bytes, class, level, mechanism }`。

---

## 2. 三個原語

| 檔案 | 角色 | API |
|---|---|---|
| `nc_mem.c` | 記憶體原語 | `nc_mem_pool_create / _init / get / put / stats`、`nc_mem_arena_create / _init / alloc / reset` |
| `nc_plan.c` | 決策 | `nc_plan_declare / commit / get / json / was_shed` |
| `nc_hub.c` | 轉手 | `nc_hub_create / _create_in`、`write_begin / write_commit`、`read_begin / read_end`、`dirty / fill` |

`_init` 系列吃「外部已配好的區域」——plan carve 出來的記憶體走這條，管理器是唯一配置者。

---

## 3. hub 的三種深度（N 是參數，不是分支）

| N | 語義 | writer 之後還能寫嗎 |
|---|---|---|
| 1 | 一次性轉交：reader 取得後長期持有 | ❌ 直到 `read_end` |
| 2 | 嚴格輪替 | ✅ 但要等 reader 歸還 |
| 3+ | 吸收抖動 | ✅ 不必等 |

狀態機：`IDLE → WRITING → READY → READING → IDLE`（多一個 `WRITING`，避免 `write_begin` 連呼兩次蓋掉自己）。

---

## 4. 不變量（程式碼層面已遵守）

1. **不等待**：所有 API 立即返回 NULL 或成功，沒有任何 `vTaskDelay`。
2. **不丟資料**：`write_begin` 只找 `IDLE` 格，永不覆蓋 `READY` / `READING`。
3. **不 sleep、不鎖**：只用臨界區保護 free list 與狀態轉移。
4. **不做政策**：skip / starve 的判斷與計數屬於用家，管理器只回報「有沒有位子」。

---

## 5. 三種記憶體形式（規劃中，slot 已可用）

| 形式 | 給什麼 | 誰分配 | 狀態 |
|---|---|---|---|
| slot | N × size | 管理器 | ✅ `nc_hub_create` |
| owner 區 | 一整塊 | 用家自己切 | ✅ `nc_mem_arena_init` |
| reserve | 「heap 至少留 N」 | 用家（WiFi 等） | ❌ 未實作 |

---

## 6. 測試

| 測試 | 位置 | 結果 |
|---|---|---|
| plan：容量充足 / 拒絕 / 撇除 / 決定性 / 窗口關閉 / 重名 | `tools/hosttest/plan_test/run.sh` | 6 場景全過 |
| hub：N=1 / N=3 / 輪替 / 自動歸還 | `tools/hosttest/hub_test/run.sh` | 全過 |
| 雙核交換（board） | `0xF003` | S3 **138k 次/秒、7.2 µs/次**；P4 **501k 次/秒、2.0 µs/次**；torn=0（P4 詳見 [p4-bringup](p4-bringup.md) §7） |

---

## 6.5 已完成（v2）

| 項目 | 內容 |
|---|---|
| 服務自己宣告 | `nc_plan_declare({service, name, level, cls, bytes, align})`；main 只呼叫服務的 declare/start |
| 每 class 一次 carve | 每個 class 只呼叫 `heap_caps_aligned_alloc` 一次，之後切 offset（host 測試檢查 alloc 次數） |
| 逐項失敗 | 不再整批失敗；配不到的標記 `ok:false`，其他人照跑 |
| hub 由 plan 供記憶體 | `nc4_parser_create_in()` / `nc_hub_create_in()` 吃 plan 的區域 |
| 退役重複模組 | `nc_buf`（queue）與 `nc_mem_cfg` 已刪除 |
| 報表 | `0x1101 keys=mem` → `{plan:{...}, hub:[...]}` |

## 7. 已知缺陷 / 待辦

| # | 項目 | 影響 |
|---|---|---|
| 1 | 無 `0x1019 ASSET_RELOAD` | 拖檔要重開機才生效 |
| 2 | 無 global 狀態（log / per-service status） | 服務無處自陳 skipped |
| 3 | 無 reserve（WiFi / 第三方 driver） | 開 WiFi 之後 heap 可能不足 |
| 4 | 服務：nc4 / uart / jobs / probe | pixel 與 WS 還沒接 |
| 5 | 慢工作已可延後（jobs hub + slow_worker） | OTA 寫 flash 還沒搬進去 |
