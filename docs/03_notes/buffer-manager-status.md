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
| `nc_hub.c` | 轉手 | `nc_hub_create / _create_in / destroy`、`write_begin / write_commit`、`read_begin / read_begin_gen / read_end`、`dirty / fill`、`generation / stats` |

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

---

## 6.6 2026-11-09 上板測試（dd46be4、IDF 6.1、app `7dc65243`）

`0xF01F`–`0xF024`、`0xF01E` 直接經 CDC 跑；全部 `torn=0`、`gap=0`、`dup=0`、`leak=0`、`viol=0`。

| 測試 | 參數 | 結果 |
|---|---|---|
| `0xF01F` stress | 20k | n=2 **165.8k/s**、n=3 35.6k/s、n=16 **239.6k/s**、n=16 慢讀 15.9k/s；`w_full` 高（環細）但無 gap/dup |
| `0xF020` duplex | 1,000 ms | a→b **156.4k/s**、b→a 156.3k/s；drain 6/1 |
| `0xF021` sweep | 500 ms × 12 | 16 B 221.9k/s、64 B 156.3k/s、256 B 69.1k/s、1 KB 21.7k/s、4 KB 5.77k/s、8 KB 2.87k/s、16 KB 1.41k/s、32 KB 1.43k/s |
| `0xF022` duplex4 | 1,000 ms | 32,768×2（128 KB SRAM）、1,244/1,250 次/s、**39.8/40.0 MB/s** |
| `0xF023` single | 20k | 16 B 253k/s、64 B 171k/s、256 B 127k/s、1 KB 46k/s、4 KB 12.8k/s（52 MB/s）、16 KB 3.3k/s（54 MB/s）、32 KB 1.69k/s（55 MB/s） |
| `0xF024` mesh | 4 nodes、1,000 ms | 12 links、**31.3k recv/s**、8 MB/s |
| `0xF01E` stage | 64 KB、4 次 | hot：s2s 244、s2p 34、p2s 51、p2p 21 MB/s；fresh：s2p 31（含 514 µs sync）、p2s 54、p2p 20；另一核 PSRAM 競爭：s2p 18、p2s 24、p2p 10 |

**觀察**

- n=3 比 n=2 慢（35.6k vs 165.8k）是排程／競爭，不是 FIFO 錯誤；torn/gap/dup 全 0。
- `0xF01E` 證實 **PSRAM 與另一核競爭時吞吐會腰斬**（21→10 MB/s p2p）；轉發走 PSRAM 要把這個算進去。
- `0xF022` 用 128 KB SRAM hub 仍有 40 MB/s；`0xF024` 4-node 12 links 的空轉很高（full/empty 約 296k），但接收 31.3k/s。

**一次 reset（未能重現）**

- 第一次連續跑 `F01F → F020 → F021 → F022 → F023(20k)` 時，`F023` 期間 USB 消失、`up_s` 重置。
- 單獨 `F023(20k)`、以及 `F022(500ms) → F023(2k)` 都正常；沒有重現。
- 可能與 `F021` 12 個 case（6 s）＋`F022` 128 KB hub 之後的 heap／排程有關；列為待查，不影響 normal hub 語義。

**工具鏈**：板子跑 IDF 6.1（app `7dc65243`）；本機預設 6.0.3 的 tests 映像 OTA 被拒
（`err_end_validate_fail`），所以這次只跑板上已有的 6.1 映像，沒有覆蓋。
