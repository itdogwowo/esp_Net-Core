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

    // 這條 bus 由哪個核心的 decode task 服務（0 / 1）。
    // **所有權原則**：driver 初始化、RX、kick、decode 全部在這個核，
    // 所以叫醒永遠是同核 notify，唔會跨核。
    int core;

    // bus 的 driver 初始化（只會在 core 上、decode loop 開始之前跑一次）。
    // 例：UART 綁 core1 → uart_driver_install() 喺 core1 執行。
    esp_err_t (*owner_init)(void *arg);
    void *owner_arg;

    // 外部提供的 parser 緩衝（plan 給的記憶體）；NULL = 自己配
    void *rx_region;
    size_t rx_region_bytes;

    // ── 以下留 0 = 用框架預設 ──
    size_t max_frame;         // 這條 bus 的 parser 緩衝（預設 NC4_MAX_FRAME）
    size_t read_chunk;        // 每輪最多讀多少 byte（預設 256）
    uint32_t budget_per_pass; // 每輪最多讀幾次，防單一 bus 餓死其他（預設 8）
    size_t rx_ring_size;      // PUSH 模式：每個插槽大小（預設 2048）；零複製要 ≥ 最大幀
    int    rx_slots;          // PUSH 模式：插槽數量（預設 4）
    // 零複製：producer 一次 commit「剛好一整幀」，consumer 直接在插槽上解析
    // （不經 slot_pop、不進 parser ring）。RING 大小要 ≥ NC4_MAX_FRAME。
    uint8_t frame_aligned;
} nc4_bus_cfg_t;

// 註冊一條通道（建立 parser / ring）。handle 可為 NULL（不需 push 時）。
esp_err_t nc4_bus_add(nc4_decoder_t *decoder, const nc4_bus_cfg_t *cfg,
                      nc4_bus_handle_t *out_handle);

// PUSH 模式入口。可在 ISR 或 task 呼叫（內部只用臨界區 + memcpy）。
// 回傳 ESP_ERR_NO_MEM 表示 ring 滿 —— 呼叫端決定丟棄或重試。
esp_err_t nc4_bus_push(nc4_bus_handle_t bus, const uint8_t *data, size_t len);

// 零複製推送：先取得可寫指標（直接 recv 進去），再 commit。
// 回傳 NULL = 目前沒有空插槽（呼叫端應該稍後再試，不要丟資料）。
uint8_t *nc4_bus_push_begin(nc4_bus_handle_t bus, size_t want);
esp_err_t nc4_bus_push_commit(nc4_bus_handle_t bus, size_t len);
// 放棄這次寫入（例如 recv 回 EAGAIN）：把插槽還回去，避免漏格
esp_err_t nc4_bus_push_abort(nc4_bus_handle_t bus);

// 清掉 parser 的半幀殘留（新連線／重連時呼叫；PUSH 模式順便丟掉所有 READY 插槽，
// 因為那些是上一條連線的資料，接在新 parser 後面只會讓它更難同步）
esp_err_t nc4_bus_reset(nc4_bus_handle_t bus);

// ── 零複製（frame_aligned=1 時給 consumer 用）──
// 取下一格整幀（不複製）；解析完呼叫 end 歸還。沒有資料回 NULL。
uint8_t *nc4_bus_frame_begin(nc4_bus_handle_t bus, size_t *len);
void nc4_bus_frame_end(nc4_bus_handle_t bus);

// producer commit 之後呼叫：叫醒服務這條 bus 的 decode task（不用等輪詢）
void nc4_bus_kick(nc4_bus_handle_t bus);

// PUSH 模式的格大小／格數（由 nc4_bus_cfg_t 決定）。
// receiver task 要知「一次最多搬幾多」—— 以前寫死一個 macro，config 改 slot_size
// 之後 reader 仍然用舊值，會直接寫壞插槽的邊界。讓 bus 自己答，只有一個真相。
size_t nc4_bus_slot_size(nc4_bus_handle_t bus);
int    nc4_bus_slot_count(nc4_bus_handle_t bus);

// 停滯偵測（monitor 用）：decode 迴圈跑過幾多次、每條 bus 的狀態。
uint32_t nc4_bus_loop_count(int core);
size_t   nc4_bus_dump(char *buf, size_t cap);

// 統計（JSON 陣列）：frames / mb / copied / avg_b / hwm / parse_ms / us_per_frame / slots
// copied = 為了把 payload 交給 parser 而做的 memcpy bytes（零複製模式應為 0）
// hwm    = receiver task 的 stack 剩餘最少 bytes（用來把 stack 調到合身）
size_t nc4_bus_stats_json(char *buf, size_t cap);
void nc4_bus_stats_reset(void);

// 登記這條 bus 的 receiver task（只為量 stack 高水位；可選）
void nc4_bus_set_producer(nc4_bus_handle_t bus, void *task_handle);

// 登記這條 bus 的 rx 參數有幾多項來自 config（0 = 全用 code 預設）。只在報表用。
void nc4_bus_set_cfg_src(nc4_bus_handle_t bus, int applied);

// 啟動這條 bus 的 producer（PUSH 模式）：
//   同一個 core 上「讀 driver → push 插槽 → kick decode」，所以叫醒係同核 notify。
// read 必須係「無限等」語義（blocking），唔可以用 timeout —— 否則又會經 core0 tick。
esp_err_t nc4_bus_start_producer(nc4_bus_handle_t bus, const char *name,
                                 uint32_t prio, uint32_t stack);

// 解碼任務本體（公開給任務註冊表使用：decode0 / decode1 兩個 loop）
// arg 必須轉型成 int 傳入「這個 task 負責的 core」
void nc4_bus_task_entry(void *arg);

size_t nc4_bus_count(void);

#ifdef __cplusplus
}
#endif
