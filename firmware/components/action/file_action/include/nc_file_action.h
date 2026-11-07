// NC4 file 域的唯讀子集 + FAT 狀態切換。
//
// 實作範圍（對齊 mp_Net-Core 的 slave/schema/*.json）：
//   0x2005 FILE_QUERY   → 0x2006 FILE_QUERY_RSP   （任何模式都答得到）
//   0x1103 STATUS_SET   → 0x1104 STATUS_ACK       （System.file_mode 0/1）
//   讀不到時回 0x2010 FILE_ERROR_RSP（err_busy=1）
//
// 讀寫分離：
//   讀 → 主機擁有 FAT 時走唯讀視圖（回覆 pending=1，可能過期/半截）
//   寫 → 才需要 STATUS_SET {"System":{"file_mode":1}} 接管
#pragma once

#include "nc4.h"
#include "nc_action.h"

#ifdef __cplusplus
extern "C" {
#endif

// 指令表（靜態）：由 main 交給 nc_action_init()。
extern const nc_action_table_t nc_file_action_table;
// 只做非註冊的初始化（psa_crypto_init）。
void nc_file_action_init(void);

#ifdef __cplusplus
}
#endif
