// NCSTATE key id：全專案唯一 source of truth（docs/03_notes/state-storage-format.md 登記）。
//
// 命名規矩：key id = domain(4b) | unit(4b) | index(4b) | field(4b)。
// 呢個 header 只放「生成期決定、執行期讀」嘅 key；新 key 先喺文件登記再加。
#pragma once

#include <stdint.h>

enum {
    // sys：0x0xxx
    NC_KEY_SYS_FRAME_MS    = 0x0002,
    NC_KEY_SYS_NUM_PIXELS  = 0x0003,
    NC_KEY_SYS_LOG_DEBUG   = 0x0004,

    // hardware I80（第一條）：0x07xx
    NC_KEY_I80_COUNT       = 0x0701,   // data 中真正用到的腳數
    NC_KEY_I80_WR          = 0x0702,
    NC_KEY_I80_DC          = 0x0703,
    NC_KEY_I80_CS          = 0x0704,
    NC_KEY_I80_FREQ        = 0x0705,

    // driver LED：0x0Cxx
    NC_KEY_LED_ENABLE      = 0x0C01,
    NC_KEY_LED_BACKEND     = 0x0C02,
    NC_KEY_LED_BUS         = 0x0C03,
    NC_KEY_LED_LANES       = 0x0C04,
    NC_KEY_LED_PX          = 0x0C05,
    NC_KEY_LED_FMT         = 0x0C06,
    NC_KEY_LED_CLK         = 0x0C07,
    NC_KEY_LED_BIT_CELLS   = 0x0C08,
    NC_KEY_LED_T0H         = 0x0C09,
    NC_KEY_LED_T1H         = 0x0C0A,
};
