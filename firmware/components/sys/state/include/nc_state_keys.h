// NCSTATE key id／unit id：全專案唯一 source of truth。
//
// **本檔由 tools/gen_state_keys.py 生成，唔好手改**：改 schema/state-keys.json 再跑工具。
// run_all.sh 會用 --check 驗同步；命名規矩：
//   key id(u32) = domain(8b) | unit(8b) | index(8b) | field(8b)
//   例：0x00070005 = domain 0（hw）／unit 7（i80）／index 0／field 5（freq）
//   index 用嚟分通道／lane／pin 編號（0..255），所以 PIN 48 支、P4 多 bus 都放得落。
#pragma once

#include <stdint.h>

#define NC_STATE_KEYS_VERSION 2u

// unit = 能力群組；key id 的 unit byte 一定要等於呢個值（生成器已驗）。
enum {
    NC_UNIT_SYS = 0,  /* 0x0 系統層（system.json） */
    NC_UNIT_I80 = 7,  /* 0x7 I80／LCD_CAM 匯流排（hardware.json） */
    NC_UNIT_SPI = 8,  /* 0x8 SPI 匯流排（hardware.json） */
    NC_UNIT_LED = 12,  /* 0xc 燈鏈（driver.json 的 LED 段） */
};

enum {
    /* 0x00000002 節拍間隔 ms（system.json:frame_interval_ms） */
    NC_KEY_SYS_FRAME_MS = 0x00000002u,
    /* 0x00000003 總像素數（資訊用）（system.json:num_pixels） */
    NC_KEY_SYS_NUM_PIXELS = 0x00000003u,
    /* 0x00000004 log 除錯模式（system.json:log_debug_mode） */
    NC_KEY_SYS_LOG_DEBUG = 0x00000004u,
    /* 0x00070001 真正接燈的資料腳數（稀疏非 -1）（hardware.json:I80.list[0].data） */
    NC_KEY_I80_COUNT = 0x00070001u,
    /* 0x00070002 WR pin（-1 = 未用）（hardware.json:I80.list[0].wr） */
    NC_KEY_I80_WR = 0x00070002u,
    /* 0x00070003 DC pin（-1 = 未用）（hardware.json:I80.list[0].dc） */
    NC_KEY_I80_DC = 0x00070003u,
    /* 0x00070004 CS pin（-1 = 未用）（hardware.json:I80.list[0].cs） */
    NC_KEY_I80_CS = 0x00070004u,
    /* 0x00070005 LCD PCLK Hz（hardware.json:I80.list[0].freq） */
    NC_KEY_I80_FREQ = 0x00070005u,
    /* 0x00080001 SPI 是否啟用（hardware.json:SPI.enable） */
    NC_KEY_SPI_ENABLE = 0x00080001u,
    /* 0x00080002 SPI 時鐘（hardware.json:SPI.list[0].baudrate） */
    NC_KEY_SPI_BAUD = 0x00080002u,
    /* 0x00080003 phase（0/1）（hardware.json:SPI.list[0].phase） */
    NC_KEY_SPI_PHASE = 0x00080003u,
    /* 0x00080004 polarity（0/1）（hardware.json:SPI.list[0].polarity） */
    NC_KEY_SPI_POL = 0x00080004u,
    /* 0x00080005 SCLK pin（hardware.json:SPI.list[0].GPIO.sck） */
    NC_KEY_SPI_SCK = 0x00080005u,
    /* 0x00080006 MOSI（單線 / data0）（hardware.json:SPI.list[0].GPIO.mosi） */
    NC_KEY_SPI_MOSI = 0x00080006u,
    /* 0x00080007 MISO（-1 = 冇）（hardware.json:SPI.list[0].GPIO.miso） */
    NC_KEY_SPI_MISO = 0x00080007u,
    /* 0x000c0001 燈鏈啟用（driver.json:LED.enable） */
    NC_KEY_LED_ENABLE = 0x000c0001u,
    /* 0x000c0002 nc_led_backend_t（driver.json:LED.backend） */
    NC_KEY_LED_BACKEND = 0x000c0002u,
    /* 0x000c0003 引用 hardware 同類 list 的 index（driver.json:LED.bus.index） */
    NC_KEY_LED_BUS = 0x000c0003u,
    /* 0x000c0004 真的接燈的 lane 數（driver.json:LED.lanes） */
    NC_KEY_LED_LANES = 0x000c0004u,
    /* 0x000c0005 每條 lane 幾顆（driver.json:LED.px） */
    NC_KEY_LED_PX = 0x000c0005u,
    /* 0x000c0006 ws2812/ws2812w/apa102…（driver.json:LED.format） */
    NC_KEY_LED_FMT = 0x000c0006u,
    /* 0x000c0007 I80 PCLK / SPI 時鐘（driver.json:LED.timing.pclk_hz） */
    NC_KEY_LED_CLK = 0x000c0007u,
    /* 0x000c0008 每 bit 幾個 cell（driver.json:LED.timing.bit_cells） */
    NC_KEY_LED_BIT_CELLS = 0x000c0008u,
    /* 0x000c0009 0 的高電平 cell 數（driver.json:LED.timing.t0h） */
    NC_KEY_LED_T0H = 0x000c0009u,
    /* 0x000c000a 1 的高電平 cell 數（driver.json:LED.timing.t1h） */
    NC_KEY_LED_T1H = 0x000c000au,
};
