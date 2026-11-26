// NCSTATE key id：全專案唯一 source of truth。
//
// **本檔由 tools/gen_state_keys.py 生成，唔好手改**：改 schema/state-keys.json，
// 然後跑 `python3 tools/gen_state_keys.py`；run_all.sh 會用 --check 驗同步。
// 命名規矩：key id = domain(4b) | unit(4b) | index(4b) | field(4b)。
#pragma once

#include <stdint.h>

#define NC_STATE_KEYS_VERSION 1u

enum {
    /* 0x0002 節拍間隔 ms（system.json:frame_interval_ms） */
    NC_KEY_SYS_FRAME_MS = 0x0002,
    /* 0x0003 總像素數（資訊用）（system.json:num_pixels） */
    NC_KEY_SYS_NUM_PIXELS = 0x0003,
    /* 0x0004 log 除錯模式（system.json:log_debug_mode） */
    NC_KEY_SYS_LOG_DEBUG = 0x0004,
    /* 0x0701 真正接燈的資料腳數（稀疏非 -1）（hardware.json:I80.list[0].data） */
    NC_KEY_I80_COUNT = 0x0701,
    /* 0x0702 WR pin（-1 = 未用）（hardware.json:I80.list[0].wr） */
    NC_KEY_I80_WR = 0x0702,
    /* 0x0703 DC pin（-1 = 未用）（hardware.json:I80.list[0].dc） */
    NC_KEY_I80_DC = 0x0703,
    /* 0x0704 CS pin（-1 = 未用）（hardware.json:I80.list[0].cs） */
    NC_KEY_I80_CS = 0x0704,
    /* 0x0705 LCD PCLK Hz（hardware.json:I80.list[0].freq） */
    NC_KEY_I80_FREQ = 0x0705,
    /* 0x0801 SPI 是否啟用（hardware.json:SPI.enable） */
    NC_KEY_SPI_ENABLE = 0x0801,
    /* 0x0802 SPI 時鐘（hardware.json:SPI.list[0].baudrate） */
    NC_KEY_SPI_BAUD = 0x0802,
    /* 0x0803 phase（0/1）（hardware.json:SPI.list[0].phase） */
    NC_KEY_SPI_PHASE = 0x0803,
    /* 0x0804 polarity（0/1）（hardware.json:SPI.list[0].polarity） */
    NC_KEY_SPI_POL = 0x0804,
    /* 0x0805 SCLK pin（hardware.json:SPI.list[0].GPIO.sck） */
    NC_KEY_SPI_SCK = 0x0805,
    /* 0x0806 MOSI（單線 / data0）（hardware.json:SPI.list[0].GPIO.mosi） */
    NC_KEY_SPI_MOSI = 0x0806,
    /* 0x0807 MISO（-1 = 冇）（hardware.json:SPI.list[0].GPIO.miso） */
    NC_KEY_SPI_MISO = 0x0807,
    /* 0x0c01 燈鏈啟用（driver.json:LED.enable） */
    NC_KEY_LED_ENABLE = 0x0c01,
    /* 0x0c02 i80/spi/rmt/parlio/direct/mock（driver.json:LED.backend） */
    NC_KEY_LED_BACKEND = 0x0c02,
    /* 0x0c03 引用 hardware 同類 list 的 index（driver.json:LED.bus.index） */
    NC_KEY_LED_BUS = 0x0c03,
    /* 0x0c04 真的接燈的 lane 數（driver.json:LED.lanes） */
    NC_KEY_LED_LANES = 0x0c04,
    /* 0x0c05 每條 lane 幾顆（driver.json:LED.px） */
    NC_KEY_LED_PX = 0x0c05,
    /* 0x0c06 ws2812/ws2812w/apa102…（driver.json:LED.format） */
    NC_KEY_LED_FMT = 0x0c06,
    /* 0x0c07 I80 PCLK / SPI 時鐘（driver.json:LED.timing.pclk_hz） */
    NC_KEY_LED_CLK = 0x0c07,
    /* 0x0c08 每 bit 幾個 cell（driver.json:LED.timing.bit_cells） */
    NC_KEY_LED_BIT_CELLS = 0x0c08,
    /* 0x0c09 0 的高電平 cell 數（driver.json:LED.timing.t0h） */
    NC_KEY_LED_T0H = 0x0c09,
    /* 0x0c0a 1 的高電平 cell 數（driver.json:LED.timing.t1h） */
    NC_KEY_LED_T1H = 0x0c0a,
};
