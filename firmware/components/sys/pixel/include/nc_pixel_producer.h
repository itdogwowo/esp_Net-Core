// nc_pixel_producer.h — 內建效果生產者（"rgbmatrix"）
//
// 這是**生產者**，不是渲染器：它只做一件事 —— 算出 R,G,B,W 四位畫面，
// 寫進環的一格。送燈、對齊、時序、DMA 全部是後端的事，這裡一概不知道。
//
// 為什麼要分開（結論來自 repo 的 `slave` 參考實作）：
//   生產者不斷提供緩衝，渲染器按指定格式讀取緩衝，兩者分開傳遞。
//   效果改壞了，燈最多是黑的；環寫錯一格，也只是那一格被跳過。
//
// 取代舊的 `rgbmatrix` 任務（dev/test_rgb_led_matrix_tasks）：
//   · 舊版把「效果 + 抖動 + I80 編碼 + DMA 送出」綁在同一個 450 fps 迴圈裡；
//   · 現在效果 60 fps 更新一次（畫面本來就不需要更快），渲染器 450 fps 照樣重播最新一格。
//     ⇒ 效果邏輯便宜 7.5 倍，而燈的更新率完全冇少。
//
// 生產者**不配置記憶體**：緩衝是 plan 的資源，由 nc_pixel_task.c 宣告與綁定。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// ── 板子的形狀（Waveshare ESP32-S3-Matrix = 8×8）──
// 欄列只影響「斜向波浪」與「第一區」的排法；換一塊板改這裡，或者用
// nc_px_producer_cfg_t 覆寫。
#define NC_PX_PRD_W        8
#define NC_PX_PRD_H        8
#define NC_PX_PRD_N        (NC_PX_PRD_W * NC_PX_PRD_H)
#define NC_PX_PRD_ZONES    8       // 純邏輯分區（第 k 區 = 鏈上第 k*8 .. k*8+7 顆）
#define NC_PX_PRD_ZONE_N   (NC_PX_PRD_N / NC_PX_PRD_ZONES)

// rate / phase 上限（舊 API 用 uint8，行為要一致）
#define NC_PX_PRD_RATE_MAX 255

typedef enum {
    NC_PX_EFF_OFF = 0,     // 全黑
    NC_PX_EFF_ZONE1 = 1,   // 只亮第一區：8 顆由暗到亮（檢查漏顆／沾色）
    NC_PX_EFF_BREATH = 2,  // RGB 輪流呼吸（breath_ms 一段，R→G→B）
    NC_PX_EFF_WAVE = 3,    // 斜向波浪＋純色流動
    NC_PX_EFF_SOLID = 4,   // 單色（frac 再加 1/256 級，做暗階 A/B）
    NC_PX_EFF_RAMP = 5,    // 全部 64 顆由 0 到最亮的一條線
    NC_PX_EFF_COUNT,
} nc_px_effect_t;

// 生產者設定。**全部欄位都可以在執行期改**（0xF060 / 0xF061）；
// 只有「幾顆燈」是板子事實（由 Kconfig 的 NETCORE_PIXEL_LEDS 給）。
typedef struct {
    uint8_t  effect;                     // nc_px_effect_t（全區預設）
    uint8_t  dither;                     // 0 off / 1 temporal / 2 spatial（全區預設）
    uint8_t  r, g, b;                    // SOLID：**顯示值** 0..255（唔經 gamma）
    uint8_t  frac;                       // SOLID：再加幾多 1/256 級（0..255）
    uint8_t  gain;                       // 0..255 整體亮度（linear，之後過 gamma）
    uint8_t  bits;                       // 模擬位數 8..16（每級 = 1/2^(bits-8) 個 8-bit 級）
    uint8_t  zone_bits[NC_PX_PRD_ZONES]; // 每區覆蓋；0 = 跟 bits
    uint8_t  zone_eff[NC_PX_PRD_ZONES];  // 每區效果；0xFF = 跟 effect
    uint8_t  zone_dither[NC_PX_PRD_ZONES]; // 每區抖動模式；0xFF = 跟 dither
    uint8_t  zone_rate[NC_PX_PRD_ZONES];   // 每區幾幀更新一次抖動（1 = 每幀）
    uint8_t  zone_phase[NC_PX_PRD_ZONES];  // 每區相位
    uint8_t  zone_on;                    // bit k = 第 k 區有輸出（其餘黑）
    uint16_t breath_ms;                  // 呼吸每段毫秒（預設 4000；太小會被夾到 200）
    uint16_t wave_ms;                    // 波浪掃一個空間週期幾多毫秒（預設 3000）
    uint16_t fps;                        // 生產者更新率（預設 60；**唔係**送燈幀率）
    uint8_t  sim6;                       // 1 = 左右 × 3 橫帶，每帶唔同抖動頻率（旁證用）
    uint8_t  sim_rate[6];                // sim6 的 6 個 rate（預設 1,2,3,4,6,8）
} nc_px_producer_cfg_t;

// ── 生命週期 ──
// 開機預設（gain 60／12 位／呼吸 4 s／demo 開）。**先呼叫它，再 set_cfg 覆寫。**
void nc_px_producer_defaults(void);

// 綁定：把生產者的 payload 與表頭接到渲染器交付的環那一格（nc_pixel_task.c 叫）。
// buf 至少要 px × 4 B；hdr 是該格的表頭（24 B）。重複 bind 是安全的。
esp_err_t nc_px_producer_bind(uint8_t *buf, uint32_t px, void *hdr, uint32_t region_bytes);
bool      nc_px_producer_ready(void);

// 算一格：把目前時間的效果寫進 buffer，並更新表頭（gen 之類）。
// 回 false = 還沒有綁定（呼叫端不應該 commit）。
// 生產者不呼叫 nc_px_ring_commit —— 誰擁有環，誰 commit（單一 writer）。
bool nc_px_producer_frame(int64_t now_us);

// 重設抖動誤差到「位置決定」的初始相位（換格式／換區大小之後呼叫）。
void nc_px_producer_reset(void);

// ── 設定 ──
nc_px_producer_cfg_t nc_px_producer_cfg(void);
void                 nc_px_producer_set_cfg(const nc_px_producer_cfg_t *cfg);

// 舊 rgbmatrix 的那組 setter（0xF060 一個一個對應）。
// **任何手動 setter 都會關掉自動 demo**（demo 是「原廠導覽」，手動就等於接管）。
void    nc_px_producer_set_effect(uint8_t effect);
void    nc_px_producer_set_dither_mode(uint8_t mode);      // 0/1/2；也接受舊的 bool
void    nc_px_producer_set_color(uint8_t r, uint8_t g, uint8_t b);
void    nc_px_producer_set_gain(uint8_t gain);
void    nc_px_producer_set_fps(uint16_t fps);              // 生產者更新率
void    nc_px_producer_set_zone(int idx, uint8_t rate, uint8_t phase);
void    nc_px_producer_set_zone_mask(uint8_t mask);
void    nc_px_producer_set_bits(uint8_t bits);
void    nc_px_producer_set_zone_bits(int idx, uint8_t bits);
void    nc_px_producer_set_zone_bits_all(const uint8_t bits[NC_PX_PRD_ZONES]);
void    nc_px_producer_set_zone_effect(int idx, uint8_t effect);
void    nc_px_producer_set_zone_effects_all(const uint8_t eff[NC_PX_PRD_ZONES]);
void    nc_px_producer_set_fraction(uint8_t frac8);
void    nc_px_producer_set_zone_dither_all(const uint8_t mode[NC_PX_PRD_ZONES]);
void    nc_px_producer_set_speed(uint16_t breath_ms);
void    nc_px_producer_set_wave_speed(uint16_t wave_ms);

// 自動 demo：開機預設開，每段 90 s 循環（呼吸／波浪）。
// 目的係唔使打字都睇得到「8/10/12 位抖動」在同一塊板上並排的效果。
void    nc_px_producer_demo(bool on);
uint8_t nc_px_producer_stage(void);      // 目前第幾段；0xFF = demo 關咗

// ── 狀態 ──
size_t nc_px_producer_json(char *out, size_t cap);   // 0xF060 回覆與 0xF061 的 "producer"

// ── 區段列表（0xF061 用：前端要知每一區現在是幾位、幾 Hz）──
typedef struct {
    uint16_t frames;      // 算過幾格
    uint32_t fps_x10;     // 實測更新率
    uint32_t render_us;   // 最近一次 render+抖動的時間
    uint32_t late;        // 節拍遲到次數
} nc_px_producer_stat_t;

void nc_px_producer_stats(nc_px_producer_stat_t *out);

#ifdef __cplusplus
}
#endif
