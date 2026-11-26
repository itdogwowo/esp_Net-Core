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
    // esp_lcd 版：D0..D(width-1)、WR、DC（width=8 → 10 個；16 → 18 個）。
    // 全部要有效 GPIO；16-bit 模式的資料線次序 = D0..D15。
    // DIRECT 版唔用呢個（可以 NULL）。
    const int *pins;
    // DIRECT 版：真正接燈的 lane 腳（lanes 個）。只會 route 這幾支，
    // 所以 WR／DC／未接燈的 data 線都唔需要真 GPIO。
    const int *lane_pins;
    uint8_t    lanes;       // DIRECT 版用；0 當 1
    // 可選：真熒幕（LCD 面板）模式。>=0 就 route，-1 = 唔用（純 LED lane 模式）。
    // ⚠ 唔可以因為 LED 用唔到 WR／DC 就把這個驅動「異化」成只for燈：
    //   同一個 nc_i80_drv_* 契約要留給將來的面板輸出（見 pixel-i80-pin-schema.md §8.4）。
    //   · wr_pin：i80 的 WR／PCLK（LED 模式唔 route；面板要）
    //   · dc_pin：LCD 的 DC（LED 模式唔需要；esp_lcd 版必須）
    //   · cs_pin：>=0 才拉（純軟件 GPIO，同 esp_lcd 一樣）；-1 = 無 CS
    //   ⚠ 全部可選：LED 模式唔填（-1）完全冇問題；面板模式填幾多就用幾多，
    //     唔會像 esp_lcd 那樣「wr／dc 缺一就整條 lane 唔建立」。
    int        wr_pin;
    int        dc_pin;
    int        cs_pin;
    bool       cs_active_high;
    // 面板 cmd phase：cmd_bits = 一個命令幾 bit（0 當 8）；DC 四段電平（0/1）
    uint8_t    cmd_bits;
    uint8_t    dc_idle_level;
    uint8_t    dc_cmd_level;
    uint8_t    dc_dummy_level;
    uint8_t    dc_data_level;
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
// 面板交易：lcd_cmd >= 0 → cmd phase＋param phase；lcd_cmd < 0 → 純資料（同 submit 等價）。
// bytes > 0 時用 bytes，0 用 slot_bytes。
bool      nc_i80_drv_submit_cmd(int slot, int lcd_cmd, size_t bytes);
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
