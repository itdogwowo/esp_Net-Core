// nc_px_engine.h — 效果引擎（工具）的契約與 API。
//
// 誰住這裡：**引擎**（契約、registry、program interpreter、波表、抖動、emit）。
// 誰**唔**住這裡：具體效果（住 `components/effects/<域>_effects/`，只 include 這支 header）。
// 規則見 docs/03_notes/behavior-taxonomy.md §2.0（三個名）與 §3（引擎 vs 實作）。
//
// 一幀怎麼走：
//   引擎逐區 → 找 effect desc → effect 寫「8.8 顯示值畫布」（px × 3 × uint16）
//   → 引擎量化＋抖動 → 8-bit → emit 成 canonical R,G,B,W 寫進環的一格。
// 效果唔知 lane、唔知 W 通道、唔寫環、唔開 task、唔碰硬件。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// ═══════════════════════ 畫布 ═══════════════════════
// 板子的形狀（Waveshare ESP32-S3-Matrix = 8×8）。分 8 個純邏輯區：第 k 區 =
// 鏈上第 k*8 .. k*8+7 顆，每區可以獨立揀效果／位數／抖動。
// （M3 之後畫布大小由環 px 決定；到時這幾個常數變成預設值。）
#define NC_PX_CANVAS_W        8
#define NC_PX_CANVAS_H        8
#define NC_PX_CANVAS_N        (NC_PX_CANVAS_W * NC_PX_CANVAS_H)
#define NC_PX_CANVAS_ZONES    8
#define NC_PX_CANVAS_ZONE_N   (NC_PX_CANVAS_N / NC_PX_CANVAS_ZONES)
#define NC_PX_CANVAS_RATE_MAX 255

// 內建效果 id（DATA program 將來會有自己的 id 區間）。
typedef enum {
    NC_PX_EFF_OFF = 0,     // 全黑
    NC_PX_EFF_ZONE1 = 1,   // 只亮第一區：8 顆由暗到亮（檢查漏顆／沾色）
    NC_PX_EFF_BREATH = 2,  // RGB 輪流呼吸
    NC_PX_EFF_WAVE = 3,    // 斜向波浪＋純色流動
    NC_PX_EFF_SOLID = 4,   // 單色
    NC_PX_EFF_RAMP = 5,    // 全部由 0 到最亮的一條線
    NC_PX_EFF_COUNT,
} nc_px_effect_t;

// ═══════════════════════ 效果契約 ═══════════════════════
// 效果收到的**全部**輸入；冇全域、冇 time()、冇 cfg 指標。
typedef struct {
    uint32_t px;          // 這塊（子）畫布幾顆
    uint16_t w, h;        // 幾何；px == w * h；一維鏈 h == 1
    uint16_t zone;        // 第幾區（0 起）——空間錯位用；效果唔應該假設固定 8 區
    int64_t  now_us;      // 引擎累加的相位時間（唔係 wall clock），唯一時間來源
    const void *params;   // nc_px_engine_params_t*（只讀，由引擎準備）
} nc_px_engine_in_t;

// 效果參數（v1：所有內建效果共用一份；M4 起由 DATA 注入）。
typedef struct {
    uint8_t  gain;        // 0..255 整體亮度（linear，之後過 gamma）
    uint8_t  r, g, b;     // SOLID：顯示值 0..255
    uint8_t  frac;        // SOLID：再加幾多 1/256 級
    uint16_t breath_ms;   // 呼吸每段毫秒
    uint16_t wave_ms;     // 波浪掃一個空間週期幾毫秒
} nc_px_engine_params_t;

// 效果寫入的是**8.8 顯示值畫布**：rgb[px*3]，每個 uint16 = 顯示 0..255 × 256。
// 抖動／量化／W 通道係引擎的事，所以效果唔會自己「跳格」。
typedef struct {
    const char *name;
    uint8_t id;                                                          // nc_px_effect_t
    void (*render)(void *st, const nc_px_engine_in_t *in, uint16_t *rgb);
} nc_px_engine_desc_t;

// ═══════════════════════ 引擎工具（tables）═══════════════════════
// 效果只可以經這幾個純函數用表；唔可以直接掂引擎內部狀態。
void     nc_px_engine_tables_init(void);
uint16_t nc_px_engine_gamma(uint16_t lin16);                            // 線性 16-bit → 8.8 顯示
uint16_t nc_px_engine_lin(const nc_px_engine_params_t *p, uint32_t level255);  // 0..255 × gain → 16-bit
// **正弦只有一個實作**：nc_px_engine_sin_lin()（256 錨點＋線性插值，514 B 表）。
// 舊的兩個方法（1024 點 Q15 查表 `nc_px_engine_sin`、MP 多項式 `nc_px_engine_sin_q12`）
// 已經移除 —— 三個並存會令人每次都要問「而家用邊個」。實測（sin_bench）：
// sin_lin 誤差 0.72 LSB、比多項式（3.74）準 5.2×、比 1024 點查表（13.31）準 18×。
// 需要 Q15 有號正弦的地方自己換算：q15 = ((int32_t)sin_lin(phase1024 << 6) - 2048) << 4。
uint16_t nc_px_engine_sin_lin(uint32_t phase);                          // 0..4095 sine（phase 0..65535）
uint16_t nc_px_engine_quant(uint16_t tgt88, uint8_t bits);              // 8.8 → bits 位（8..16）

// ═══════════════════════ 效果註冊表 ═══════════════════════
// 實作 component 開機時把 desc 交入來（`nc_pixel_effects_register()`）；
// 引擎唔知道有邊幾個效果，只認 id。
void                        nc_px_engine_register(const nc_px_engine_desc_t *d);
const nc_px_engine_desc_t  *nc_px_engine_find(uint8_t id);
int                         nc_px_engine_fx_count(void);

// ═══════════════════════ 引擎設定 ═══════════════════════
// **全部欄位都可以在執行期改**（0xF060／0xF061）。
typedef struct {
    uint8_t  effect;                     // nc_px_effect_t（全區預設）
    uint8_t  dither;                     // 0 off / 1 temporal / 2 spatial（全區預設）
    uint8_t  r, g, b;                    // SOLID：顯示值 0..255（唔經 gamma）
    uint8_t  frac;                       // SOLID：再加幾多 1/256 級（0..255）
    uint8_t  gain;                       // 0..255
    uint8_t  bits;                       // 模擬位數 8..16
    uint8_t  zone_bits[NC_PX_CANVAS_ZONES];   // 每區覆蓋；0 = 跟 bits
    uint8_t  zone_eff[NC_PX_CANVAS_ZONES];    // 每區效果；0xFF = 跟 effect
    uint8_t  zone_dither[NC_PX_CANVAS_ZONES]; // 0xFF = 跟 dither
    uint8_t  zone_rate[NC_PX_CANVAS_ZONES];   // 每區幾幀更新一次抖動（1 = 每幀）
    uint8_t  zone_phase[NC_PX_CANVAS_ZONES];  // 每區相位
    uint8_t  zone_on;                    // bit k = 第 k 區有輸出
    uint16_t breath_ms;
    uint16_t wave_ms;
    uint16_t fps;                        // 生產者更新率（唔係送燈幀率）
    uint8_t  sim6;                       // 1 = 左右 × 3 橫帶（旁證用）
    uint8_t  sim_rate[6];
} nc_px_engine_cfg_t;

typedef struct {
    uint16_t frames;
    uint32_t fps_x10;
    uint32_t render_us;
    uint32_t late;
} nc_px_engine_stat_t;

// ═══════════════════════ 生命週期 ═══════════════════════
// 開機預設。**先呼叫它，再 set_cfg 覆寫。**
void      nc_px_engine_defaults(void);

// 綁定：只記住「這條 lane 幾顆」，唔持有環的任何一格。
esp_err_t nc_px_engine_bind(uint32_t px);
bool      nc_px_engine_ready(void);

// 算一格：寫進呼叫端剛 begin 出來的那一格（dst = px × 4 B，hdr = 24 B）。
bool nc_px_engine_frame(uint8_t *dst, void *hdr, int64_t now_us);

// 把上一格抄到另一條 lane（多條燈鏈同一個畫面）。
bool nc_px_engine_copy(uint8_t *dst, void *hdr, uint32_t capacity_bytes);

void nc_px_engine_reset(void);

// ═══════════════════════ 設定（0xF060 逐個對應）═══════════════════════
nc_px_engine_cfg_t nc_px_engine_cfg(void);
void               nc_px_engine_set_cfg(const nc_px_engine_cfg_t *cfg);

void    nc_px_engine_set_effect(uint8_t effect);
void    nc_px_engine_set_dither_mode(uint8_t mode);      // 0/1/2；也接受舊的 bool
void    nc_px_engine_set_color(uint8_t r, uint8_t g, uint8_t b);
void    nc_px_engine_set_gain(uint8_t gain);
void    nc_px_engine_set_fps(uint16_t fps);
void    nc_px_engine_set_zone(int idx, uint8_t rate, uint8_t phase);
void    nc_px_engine_set_zone_mask(uint8_t mask);
void    nc_px_engine_set_bits(uint8_t bits);
void    nc_px_engine_set_zone_bits(int idx, uint8_t bits);
void    nc_px_engine_set_zone_bits_all(const uint8_t bits[NC_PX_CANVAS_ZONES]);
void    nc_px_engine_set_zone_effect(int idx, uint8_t effect);
void    nc_px_engine_set_zone_effects_all(const uint8_t eff[NC_PX_CANVAS_ZONES]);
void    nc_px_engine_set_fraction(uint8_t frac8);
void    nc_px_engine_set_zone_dither_all(const uint8_t mode[NC_PX_CANVAS_ZONES]);
void    nc_px_engine_set_speed(uint16_t breath_ms);
void    nc_px_engine_set_wave_speed(uint16_t wave_ms);

// 自動 demo（原廠導覽）。任何手動 setter 都會關掉它。
void    nc_px_engine_demo(bool on);
uint8_t nc_px_engine_stage(void);        // 0xFF = demo 關咗

// ═══════════════════════ 狀態 ═══════════════════════
size_t nc_px_engine_json(char *out, size_t cap);      // 0xF060 回覆／0xF061 "producer"
void   nc_px_engine_stats(nc_px_engine_stat_t *out);

#ifdef __cplusplus
}
#endif
