// OTA（0x22xx）— 指令表與 mp_Net-Core 的 slave/schema/ota.json 一致
//
// 已實作（第一版）：2201 BEGIN / 2202 WRITE / 2203 END / 2205 ABORT /
//                   2206-2207 VERSION / 2210-2211 CAPS / 2218-2219 PROGRESS /
//                   221A-221B STATE / 221C ERROR / 2220 APPLY
// 未實作（需要自訂 slot metadata / NVS 記錄）：2212-2213 LAST、2214-2215 PARTITION_STATUS、2216-2217 VERIFY
//
// 語義都照 ota.json：欄位順序、型別、LE、str_u16len（2B 長度前綴）。
// 錯誤一律回 OTA_ERROR_RSP 的 11 個 bool 之一，不發明新的錯誤碼。

#pragma once

#include "nc4.h"
#include "nc_action.h"

#ifdef __cplusplus
extern "C" {
#endif

// 指令表（靜態）：由 main 交給 nc_action_init()。
extern const nc_action_table_t nc_ota_action_table;
// 只做非註冊的初始化（清狀態）。
void nc_ota_action_init(void);

// OTA session 期間（BEGIN 抹分區 → END 驗證完）回 true。
//
// 為什麼需要：外部任務（ncm 的 .nct 模組）每 tick 經 WAMR 由 flash 執行程式碼，
// 而同時間 OTA 正在 erase/write flash（cache 會被暫停、另一核會被 park）。
// 2026-11-17 實測：1.3 MB 映像 OTA、模組正在跑 → task watchdog 5 s 到期 → 重啟
// （rst=6），OTA 中斷。所以 engine 在 session 期間跳過 tick；模組本身不動，
// session 完咗自然繼續（BEGIN/END 之間 host 只會收 ACK，唔需要模組輸出）。
bool nc_ota_action_paused(void);

#ifdef __cplusplus
}
#endif
