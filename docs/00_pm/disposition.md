# 處置清單：哪些要移除、哪些要換新做法

> **用途**：把「核心分割」「模組邊界」兩條線上的每一項標上處置，
> 避免文件只寫「未接」而分不清是**不再做**還是**要重做**。
>
> **圖例**：
> - 🗑️ **移除**：不再需要，直接刪（程式／文件／config），避免誤導。
> - 🔄 **新做法**：方向保留，實作要換（重寫、搬位置、改來源）。
> - ✅ **保留**：現況有效，不改。
> - ❓ **待決**：需要你拍板。

---

## A. 核心分割

| # | 項目 | 現況 | 處置 | 建議 |
|---|---|---|---|---|
| A1 | 兩核共用一個 decoder（一個 mutex 序列化 handler） | 第二核只平行解析 | ❓ | 要決定：per-core decoder（兩份表＋reentrant handler）還是接受序列化 |
| A2 | `routes[].out` 的 link 轉發 | 格式存在、完全未實作 | 🔄 | **先從 `out` 的 schema／文件移除 link 名**，等真的做時再加（config 不該寫未生效的東西） |
| A3 | `outbound` 永遠 `[]` | 預留列 | ✅ | 保留，但標「未生效」 |
| A4 | `tcp_accept`／`tcp_rx`／`nc4_udp` 裸 `xTaskCreate` | 繞過任務管理器 | 🔄 | 收進 `tasks[]`，由管理器建立／管核 |
| A5 | `fx` 寫死 core1；manifest 的 `core` 未用 | 固定 placement | 🔄 | 依 manifest `core` 分組；`-1` → 預設核 |
| A6 | WS／UDP 不在 `links[]` | 在 httpd／udp task 內直接解析 | 🔄 | 納入 `links[]`，才有 enable gate／rx 區域／量測 |
| A7 | 核歸屬有 4 個來源（router／NVS／fx·WS·UDP 寫死／tasks.json） | 多真相 | 🔄 | 收斂成「宣告 → 一處決定」（見 B 方案） |
| A8 | NVS `net/rx_core` | 第二個核歸屬來源 | 🗑️ | **移除**：RX 核由 router 的 `decodeN` 推導 |
| A8b | NVS `net/rx_split`／`frame_align` | 傳輸調校（不是核歸屬） | 🔄 | 搬到 `system.json` 的 `links[].cfg`，跟其他通道參數一起 |
| A9 | `NC_TASK_CORE_ANY` | 框架支援、目前無人用 | ✅ | 保留，文件標「未使用」 |
| A10 | `claims[]` 帳本檢查 | 零使用者，audit 是 no-op | ❓ | 要嘛真的用（pin／bus 宣告），要嘛移除 audit 與相關承諾 |
| A11 | `router_defaults()` 後備表 | 讀不到 config 時用 | ✅ | 保留 |
| A12 | `level` 只影響 on_start 次序 | 不是 runtime 排序 | ✅ | 保留（文件已寫清楚） |

---

## B. 模組邊界

| # | 項目 | 現況 | 處置 | 建議 |
|---|---|---|---|---|
| B1 | `main.c` 的正式 handler（STATUS／ROUTE／REBOOT／SET_MASTER／SYS_TASK_QUERY） | 在 main.c | 🔄 | 搬去 `action/`（core action 一個檔），main 只剩裝配 |
| B2 | `0xF00x` 測試 handler（`DEV_BENCH`／`HUB_BENCH`／`TX_BENCH`／`UART_BENCH`／`DEV_SLOW`） | 常駐在正式映像 | 🗑️／🔄 | 建議**整組移除**，或搬去 `bench` 元件並用 `CONFIG_NETCORE_TEST_CMDS` gate |
| B2b | `DEV_BENCH` 的 mode 2/3/4（erase／write OTA 分區） | 任何通道都打得到 | 🗑️ | ✅ **已移除**（2026-11-04）：`on_dev_bench` 只剩 mode 0/1 |
| B3 | `NC_DECODE_CORE` 巨集 | 定義了、零使用 | 🗑️ | ✅ 已刪（router 已取代；原位留一行指向 router） |
| B4 | `nc4_bus_start()` | 定義＋宣告、零呼叫 | 🗑️ | ✅ 已刪（連 header 註解、`s_task`、`DEFAULT_TASK_STACK/PRIO`） |
| B5 | `NC4_CMD_RESET_TO_DOWNLOAD`（0x1019） | 定義了、零使用（文件已說刪） | 🗑️ | ✅ 已刪；`nc4.h` 原位留註解：0x1019 保留給 `ASSET_RELOAD` |
| B6 | `main.c` 重複 `#include "nc_task.h"`（L54/L63） | 重複 | 🗑️ | ✅ 已刪一行 |
| B7 | `tcp_sink`／`tcp_src` 測試 task | 已由 `CONFIG_NETCORE_TEST_TASKS` gate | ✅ | 保留（預設關） |
| B8 | `hub_bench`／`tx_bench` 用的 static hub | 只在測試 handler 內 | 🗑️ | 跟 B2 一起處理 |

---

## C. 文件

| # | 文件 | 問題 | 處置 |
|---|---|---|---|
| C1 | `buffer-manager.md` | 狀態寫「設計定案、尚未實作」，但 `buffer-manager-status.md` 說交換管理器 v2 已完成 → **互相矛盾** | 🔄 改狀態行，指向 status |
| C2 | `decode-routes.md` | 標題寫「現況 2026-11-02」，但 bus／core 已改（router、links.enable） | 🔄 更新或標「歷史快照」 |
| C3 | `architecture-flow.md` | 描述目標架構（pixel／FrameClock 未實作） | ✅ 保留，但要標「目標，不是現況」 |
| C4 | `status.md` §9 | `[rewrite-plan.md](roadmap.md)` 標籤與目標不符 | 🔄 改成 `roadmap.md` |
| C5 | `routing.md` §7 | 只有「現況／要改成」，沒有處置 | 🔄 已加「處置」欄 |
| C6 | `execution-model.md` | watchdog 已實作 | ✅ 已更新 |
| C7 | `task-fx-open-questions.md` | 決策清單 | ✅ 保留 |

---

## D. 建議的執行順序

1. ✅ **已完成（2026-11-04）**（不影響行為）：
   B3、B4、B5、B6（死碼）、B2b（flash 測試模式）、C1、C4（文件矛盾／連結）。
2. **安全但需要你決定**：
   B2（其餘 `0xF00x` bench handler）→ 整組移除，或搬去 gated 元件。
3. **改架構（較大）**：
   A8（拿掉 NVS rx_core）、A4（收編裸 task）、A6（WS/UDP 入 links）、A5（fx core）、A7（核歸屬收斂）。
4. **需要你拍板**：
   A1（per-core decoder）、A10（claims）、A2（out link 是否先移除）。

> **一句話**：程式上真正該「移除」的是**死碼與危險的測試入口**；
> 其餘大部分是「換新做法」（核歸屬收斂、裸 task 收編、action 搬家），不是刪掉。
