// NC4 匯流排框架：多通道 → 中間緩衝 → 單一解碼器
//
// 對應 mp_Net-Core 的 BusSources + BusDecodeTask：
//
//   transport A ─┐
//   transport B ─┼─► nc4_bus（每條 bus 一個 parser，黏包狀態獨立）
//   transport C ─┘        │
//                         └─► nc4_decoder（共用 dispatch 表）─► handler
//
// 兩種驅動方式，涵蓋所有通道：
//
//   PULL（解碼任務主動讀）  USB CDC / UART / TCP / WS
//        transport 提供 read()，任務輪詢
//
//   PUSH（傳輸端主動丟）    I2C slave / SPI slave / CAN / ESP-NOW
//        ISR 或 RX task 呼叫 nc4_bus_push()，任務從 ring 取
//
// ⚠️ 分段（segmentation）是 transport 的責任，不是解碼器的：
//    CAN 一則訊息只有 8（或 64）bytes，NC4 幀可以到 8KB；
//    I2C/SPI slave 一次 transaction 也有上限。
//    這些通道必須自己把碎片拼成「有序 byte stream」再 push 進來 ——
//    解碼器看到的永遠是連續位元組，它不知道底下是 CAN 還是 USB。
//
// 參數都是「框架預設值」，不是定律 —— 不同鏈路 burst 量差很多，
// 實際值應從執行期設定（config.json）餵進來。

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "nc4.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NC4_MAX_BUSES        8

typedef enum {
    NC4_BUS_PULL = 0,     // 解碼任務主動呼叫 read()
    NC4_BUS_PUSH,         // 傳輸端呼叫 nc4_bus_push()
} nc4_bus_mode_t;

typedef struct nc4_bus *nc4_bus_handle_t;

typedef struct {
    const char *name;     // "CDC" / "UART0" / "I2C1-S" / "CAN0" …

    nc4_bus_mode_t mode;

    // ── PULL 模式 ──
    // >0 = 讀到幾個 byte，0 = 目前沒資料，<0 = 錯誤
    int (*read)(void *arg, uint8_t *buf, size_t cap);
    void *read_arg;

    // ── 共用 ──
    // 回覆出口；NULL = 這條通道不支援回覆
    nc4_send_fn send;
    void *send_arg;

    // ── 以下留 0 = 用框架預設 ──
    size_t max_frame;         // 這條 bus 的 parser 緩衝（預設 NC4_MAX_FRAME）
    size_t read_chunk;        // 每輪最多讀多少 byte（預設 256）
    uint32_t budget_per_pass; // 每輪最多讀幾次，防單一 bus 餓死其他（預設 8）
    size_t rx_ring_size;      // PUSH 模式的 ring（預設 2048）
} nc4_bus_cfg_t;

// 註冊一條通道（建立 parser / ring）。handle 可為 NULL（不需 push 時）。
esp_err_t nc4_bus_add(nc4_decoder_t *decoder, const nc4_bus_cfg_t *cfg,
                      nc4_bus_handle_t *out_handle);

// PUSH 模式入口。可在 ISR 或 task 呼叫（內部只用臨界區 + memcpy）。
// 回傳 ESP_ERR_NO_MEM 表示 ring 滿 —— 呼叫端決定丟棄或重試。
esp_err_t nc4_bus_push(nc4_bus_handle_t bus, const uint8_t *data, size_t len);

// 解碼任務本體（公開給任務註冊表使用；也可用 nc4_bus_start 直接建立）
void nc4_bus_task_entry(void *arg);

// 啟動解碼任務（掃描所有已註冊通道）。等於自行建立上面那個 task。
esp_err_t nc4_bus_start(uint32_t poll_ms);

size_t nc4_bus_count(void);

#ifdef __cplusplus
}
#endif
