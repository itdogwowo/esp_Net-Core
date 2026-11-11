// 測試指令（0xF00x / 0x181x）：全部集中在這裡，受 NC_ENABLE_TESTS gate。
//
// 約定（execution-rules §6）：測試指令一律放 components/action/test_action/，
// 正式映像唔編譯。main 只負責呼叫 nc_test_action_register()。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "nc4.h"
#include "nc_action.h"

#ifdef __cplusplus
extern "C" {
#endif

// main 提供：測試指令要讀的 RX 診斷（住喺通道裝配層）
typedef struct {
    uint32_t (*rx_counter)(int idx);   // 0=resync 1=frames 2=hub_full 3=sel 4=sel_to
    uint64_t (*rx_fill_us)(void);
    bool (*frame_align)(void);
    void (*reset_rx)(void);
} nc_test_action_hooks_t;

// 指令表（靜態）：由 main 交給 nc_action_init()。
extern const nc_action_table_t nc_test_action_table;
// 設定 hooks（測試指令要讀的 RX 診斷）。
void nc_test_action_init(const nc_test_action_hooks_t *hooks);
// 測試版開機自動起 ESP-NOW（B 冇 CDC 時也可以做兩板互轉）。
void nc_test_action_espnow_boot(nc4_decoder_t *decoder);
// P4 測試版：開機後自動對 peer 跑吞吐掃描（走 0xF050 同一條路徑）
void nc_test_action_netauto_boot(void);

#ifdef __cplusplus
}
#endif
