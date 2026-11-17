// 8×8 RGB LED 矩陣（Waveshare ESP32-S3-Matrix）—— 測試任務
//
// 硬體事實（2026-11-11 由官方原理圖＋Demo 核對）：
//   · 64 顆 WS2812B-0807 串成一條鏈，資料腳 = GPIO14，Dout 可再串；
//   · 顏色次序 NEO_RGB（Demo 用 Adafruit_NeoPixel(64, 14, NEO_RGB + NEO_KHZ800)）；
//   · 板載 IMU = IO10/11/12/13，唔可以當 I80 的 dummy 腳。
//
// 為什麼用 I80：一個 PCLK 週期推一個 byte 上 8 條資料線，D0 就是 WS2812 的
// 位元流（其餘 7 條留給將來的 8-lane 燈板）。整幀由 GDMA 推，CPU 只負責編碼。
//   8.889 MHz PCLK（80 MHz ÷ 9）→ 112.5 ns/cycle；一個 WS2812 bit = 8 cycles = 0.900 µs
//   （0 碼高 3 cycles = 337 ns、1 碼高 6 = 675 ns）。
//   64 顆 × 24 bit × 8 = 12,288 B（1.41 ms）＋ 256 B 收尾 ≈ 1.65 ms／幀（含交易開銷）
//   ⇒ 全速 ~606 fps，450 fps 有 ~0.57 ms 餘裕（reset 由兩幀之間的拉低提供）。
//   為什麼不用 4 MHz／5 cycles：esp_lcd 的 I80 每筆交易有 0.24–0.45 ms 固定開銷，
//   4 MHz 一幀 1.98 ms 實測要 2.44 ms，只到 408 fps。
//
// 抖動（dither）：WS2812 每顆燈內部只有 8-bit PWM，暗階一格一跳。
// 我們把「想要的亮度」保留 8.8 定點精度，每幀用誤差回饋決定送 8-bit 的
// floor 定 ceil，人眼平均之後就等於多出 8 bit 的灰階 —— 這就是「用幀率換灰階」。
// 8 個邏輯區（每區 = 一橫行 8 顆）可以各自有不同的抖動速率／相位，
// 令整塊板不會同一個頻率一齊閃；rate 越低閃得越慢（低過 ~100 Hz 就會見到）。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "nc_task.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NC_RGB_W        8
#define NC_RGB_H        8
#define NC_RGB_N        (NC_RGB_W * NC_RGB_H)
#define NC_RGB_ZONES    8
#define NC_RGB_ZONE_N   (NC_RGB_N / NC_RGB_ZONES)

// 一個 WS2812 bit 用幾個 PCLK 週期。
// 3 cycles @ 2.5 MHz → bit 1.200 µs、T0H 0.400 µs、T1H 0.800 µs
// ——呢組就係 Waveshare demo（Adafruit_NeoPixel 800 kHz）嘅對應時序，最保守。
// 註：8.889 MHz／8 cycles（bit 0.9 µs、T0H 337 ns、T1H 675 ns）理論上仍在
// WS2812B 窗口內、而且可以做到 450 fps，但實機「完全冇光」→ 先用保守時序確認燈，
// 再一級一級加快（pclk 同 cycles 都係 Kconfig／巨集，改完 OTA 就得）。
#define NC_RGB_BIT_CYCLES  3
// 幀尾補 0x00：保證資料線拉低（256 B = 64 µs），剩下的等待時間湊夠 reset
#define NC_RGB_RESET_BYTES 256
#define NC_RGB_TX_BYTES    (NC_RGB_N * 24 * NC_RGB_BIT_CYCLES + NC_RGB_RESET_BYTES)
// 三格 DMA buffer：DMA 送第 N 幀時，CPU 已經編好 N+1、N+2。
// 單緩衝要等 DMA 完成才可以編碼 → 一幀 = 1.984 ms + 編碼 ≈ 2.45 ms（實測只 408 fps）；
// 兩格仍然會在「等完成 → 編碼 → 提交」之間讓 bus 空一陣（實測 DMA 週期 2.43 ms）；
// 三格保證任何時候都有 ≥1 幀排隊，bus 可以背靠背送。
#define NC_RGB_SLOTS       3
#define NC_RGB_BUF_BYTES   (NC_RGB_TX_BYTES * NC_RGB_SLOTS)
// 4 MHz 下一幀的傳輸時間（7936 B × 250 ns ≈ 1.98 ms）
#define NC_RGB_TX_US       1984
// 資料本身佔 1.92 ms；留 reset 之後的硬上限（超過就一定會 late）
#define NC_RGB_FPS_MAX     460

typedef enum {
    NC_RGB_EFF_OFF = 0,     // 全黑
    NC_RGB_EFF_ZONE1 = 1,   // 只亮第一區：8 顆由暗到亮，檢查有沒有漏顆／沾色
    NC_RGB_EFF_BREATH = 2,  // RGB 輪流呼吸（2.5 s 一段，R→G→B）
    NC_RGB_EFF_WAVE = 3,    // 斜向波浪＋色相流動
    NC_RGB_EFF_SOLID = 4,   // 單色（A/B 暗階用）
    NC_RGB_EFF_RAMP = 5,    // 64 顆由 0 到最亮的一條線：抖動開／關最易比較
    NC_RGB_EFF_COUNT,
} nc_rgb_effect_t;

typedef struct {
    uint8_t  effect;     // nc_rgb_effect_t
    // 抖動模式：0 = off，1 = 時間（誤差回饋，逐幀），2 = 空間（4×4 ordered，旋轉）
    uint8_t  dither;
    // SOLID：r/g/b 係**顯示值** 0..255（即係 8-bit 世界見到嘅碼，唔再過 gamma），
    // frac 係再加幾多 1/256 級（0..255）—— 用嚟做「整數碼 vs 半分碼」嘅抖動比較。
    uint8_t  r, g, b;
    uint8_t  frac;
    uint8_t  gain;       // 0..255 整體亮度（linear，再過 gamma）
    // ── 內部精度（唔用浮點：整數定點）──
    // bits = 模擬位數：8 = 原生 8-bit（冇得抖）、12 = 每級 1/16、16 = 1/256。
    // 內容先量化到 bits 位，之後每一幀用誤差回饋補回 8-bit 之間的差。
    // 位數越高 ⇒ 需要越多幀平均 ⇒ 抖動頻率越低（越易睇到閃）。
    uint8_t  bits;                     // 全域（預設 12）
    uint8_t  zone_bits[NC_RGB_ZONES];  // 每區覆蓋；0 = 跟 bits
    // 每區（8 顆）可以獨立揀效果：0xFF = 跟全域 effect。
    // 分區純邏輯：第 k 區 = 鏈上第 k*8 .. k*8+7 顆（GPIO14 一條鏈，冇額外線）。
    uint8_t  zone_eff[NC_RGB_ZONES];
    // 每區抖動模式：0 off / 1 temporal / 2 spatial；0xFF = 跟全域 dither。
    // 用嚟做「8 區 8 個唔同頻率，其中一區完全唔抖」嘅並排比較。
    uint8_t  zone_dither[NC_RGB_ZONES];
    // 模擬 fps：左右 × 3 橫帶 = 6 個區域，每區用唔同 rate（effective fps = fps/rate）。
    // sim6 = 1 時生效（demo 用），bit 固定用 10。
    uint8_t  sim6;
    uint8_t  sim_rate[6];              // 預設 {1,2,3,4,6,8} → 419/210/140/105/70/52 fps
    uint16_t breath_ms;                // 呼吸每段毫秒（預設 4000 → R/G/B 共 12 s）
    uint16_t wave_ms;                  // 波浪掃一個空間週期幾多 ms（預設 3000）
    uint16_t fps;        // 目標幀率（上限 NC_RGB_FPS_MAX）
    uint8_t  zone_rate[NC_RGB_ZONES];   // 每區幾幀更新一次抖動（1 = 每幀）
    uint8_t  zone_phase[NC_RGB_ZONES];  // 每區相位（0..rate-1 以外的值會取模）
    uint8_t  zone_on;    // bit k = 第 k 區有輸出
} nc_rgb_cfg_t;

// 任務宣告（main 在 NC_ENABLE_TESTS 下登記）。
const nc_task_t *nc_rgb_matrix_task(void);

// 啟動成功（I80 bus 起得來）才可以 call 之後那些 setter。
bool     nc_rgb_matrix_ready(void);
nc_rgb_cfg_t nc_rgb_matrix_cfg(void);

void nc_rgb_matrix_set_effect(uint8_t effect);
void nc_rgb_matrix_set_dither(bool on);
void nc_rgb_matrix_set_dither_mode(uint8_t mode);   // 0 off / 1 temporal / 2 spatial
void nc_rgb_matrix_set_color(uint8_t r, uint8_t g, uint8_t b);
void nc_rgb_matrix_set_gain(uint8_t gain);
void nc_rgb_matrix_set_fps(uint16_t fps);
void nc_rgb_matrix_set_zone(int idx, uint8_t rate, uint8_t phase);
void nc_rgb_matrix_set_zone_mask(uint8_t mask);
void nc_rgb_matrix_set_bits(uint8_t bits);            // 全域模擬位數 8..16
void nc_rgb_matrix_set_zone_bits(int idx, uint8_t bits);  // 0 = 跟全域
void nc_rgb_matrix_set_zone_bits_all(const uint8_t bits[NC_RGB_ZONES]);
void nc_rgb_matrix_set_zone_effect(int idx, uint8_t effect);   // 0xFF = 跟全域
void nc_rgb_matrix_set_zone_effects_all(const uint8_t eff[NC_RGB_ZONES]);
void nc_rgb_matrix_set_fraction(uint8_t frac8);
void nc_rgb_matrix_set_zone_dither_all(const uint8_t mode[NC_RGB_ZONES]);
void nc_rgb_matrix_set_speed(uint16_t breath_ms);     // 呼吸每段毫秒
void nc_rgb_matrix_set_wave_speed(uint16_t wave_ms);  // 波浪掃一個週期幾多 ms
// 自動 demo：開機預設開，每段 20–24 秒循環（呼吸／波浪／暗階／8 區抖動速率／8 區位數／8 區效果）。
// 任何手動 setter 會自動熄 demo，等你接管。
void nc_rgb_matrix_demo(bool on);
uint8_t nc_rgb_matrix_stage(void);

// 狀態（JSON）：給 0xF060 與 STATUS 用。
size_t nc_rgb_matrix_json(char *out, size_t cap);

#ifdef __cplusplus
}
#endif
