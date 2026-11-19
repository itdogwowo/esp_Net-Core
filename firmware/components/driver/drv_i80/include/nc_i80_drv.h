// nc_i80_drv.h — I80（LCD_CAM 8080 並口）驅動：只用 GDMA 推 byte 流。
//
// 這一層**只知道硬體**：
//   · 10 個腳位（D0..D7、WR、DC）、PCLK、一筆幾個 byte、幾個 DMA 槽；
//   · 送出去、等完成、把緩衝交回。
// 唔知道像素、燈、WS2812、環、任務 —— 那些是上層的事。
//
// 使用次序（上層 adapter 負責）：
//   install(cfg) → 拿 slot_buf(i) 填資料 → submit(i) → slot_done(i) → 可重用
//
// 腳位 claim／保留腳判斷屬政策，留在組裝層／task adapter（driver 唔 include nc_hw）。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NC_I80_DRV_SLOTS_MAX 3

typedef struct {
    // pins = D0..D(width-1)、WR、DC（width=8 → 10 個；width=16 → 18 個）。
    // 全部要有效 GPIO；16-bit 模式的資料線次序 = D0..D15。
    const int *pins;
    uint32_t   pclk_hz;     // PCLK（WS2812 的 bit 寬度靠它 × bit_cells 湊出來）
    uint32_t   slot_bytes;  // 每一筆固定送幾個 byte（16-bit 模式 = units × 2）
    uint8_t    slots;       // 2..NC_I80_DRV_SLOTS_MAX（3 = 三緩衝）
    uint8_t    bus_width;   // 8 或 16（16 = 一個 PCLK 出兩條 lane 組，時間同 8-bit 一樣）
} nc_i80_drv_cfg_t;

esp_err_t nc_i80_drv_install(const nc_i80_drv_cfg_t *cfg);
void      nc_i80_drv_uninstall(void);
bool      nc_i80_drv_ready(void);

int       nc_i80_drv_slots(void);
uint32_t  nc_i80_drv_slot_bytes(void);
uint8_t  *nc_i80_drv_slot_buf(int slot);      // DMA-capable internal；NULL = 未就緒

// 把 slot 的 slot_bytes 交去 I80。成功回 true（完成會記在 driver 內部）。
bool      nc_i80_drv_submit(int slot);
// DMA 完成了嗎？回 true 時順便清掉「完成」標記（呼叫端可以重用 slot）。
bool      nc_i80_drv_slot_done(int slot);

// 帳面（狀態 JSON 用）
uint32_t  nc_i80_drv_submitted(void);
uint32_t  nc_i80_drv_done_count(void);
uint32_t  nc_i80_drv_fail(void);
uint32_t  nc_i80_drv_inflight(void);
uint32_t  nc_i80_drv_pclk(void);

#ifdef __cplusplus
}
#endif
