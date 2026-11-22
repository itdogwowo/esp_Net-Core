// nc_render.h — 渲染任務（render task）：把環裡的幀交給後端／驅動。
//
// 分工（2026-11-18）：
//   · 效果／Map：另一個任務（tasks/pixel_task）＋ sw/pixel 的環與編碼器；
//   · **這個任務只做渲染**：取最新一格 → 編碼 → 後端 → 硬件。
//     將來 LCD 同 LED 都由同一個 render 任務處理，所以這裡刻意不放效果。
//
// 一個 render 任務可以跑多道 lane（每道 lane = 一個後端 + 一條環）；
// 一條環可以由任何生產者餵（像素效果、`.nce`、主機推送）。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nc_pixel_core.h"     // 環 + 編碼器（純 C，hosttest 同一份）
#include "esp_err.h"           // 後端註冊 API 用
#include "nc_task.h"           // 任務宣告（nc_task_t）

#ifdef __cplusplus
extern "C" {
#endif

// ── 後端上限（渲染器側）──
#define NC_PX_MAX_LANES     8        // 一個渲染器最多幾道 lane
#define NC_PX_MAX_FPS       1000     // 渲染節拍上限
#define NC_PX_BACKEND_MAX   16       // 後端名上限

// ═══════════════════════ 後端 ═══════════════════════
// 後端 = 一種驅動。它拿環裡的一格、用共用編碼器寫進自己的 DMA 槽，然後送出去。
// 渲染器永遠不知道驅動長什麼樣 —— 新增一種驅動 = 新增一個後端。
typedef struct nc_pixel_backend nc_pixel_backend_t;
struct nc_pixel_backend {
    char  name[NC_PX_BACKEND_MAX];   // "i80" / "spi" / "direct" / "mock"
    void *ctx;
    nc_pixel_q_t *ring;              // 這個後端要讀的環（nc_render_bind_lanes() 設定）
    uint8_t lanes;                   // 這個後端幾條 lane（i80 可以 1..8／16）
    uint8_t width;                   // i80 的 bus 寬度（8／16）；其他後端 = 0

    // 取一格 seq > post_seq 的。成功填 slot／payload／px／seq。
    bool (*acquire)(nc_pixel_backend_t *b, uint32_t post_seq, uint32_t *slot,
                    const uint8_t **payload, uint32_t *px, uint32_t *seq);
    // 用共用編碼器把那一格編成這個後端的物理格式。
    bool (*encode)(nc_pixel_backend_t *b, uint32_t slot);
    // 交給硬體；回 false = 沒有提交（渲染器會還格）。
    bool (*submit)(nc_pixel_backend_t *b, uint32_t slot, uint32_t seq);
    // 還「已完成」的格（DMA 完成才可以還：P6 陷阱 1）。
    void (*reclaim)(nc_pixel_backend_t *b);
    // 狀態 JSON（物件，不含外層括號）。可為 NULL。
    size_t (*json)(nc_pixel_backend_t *b, char *out, size_t cap);
};

esp_err_t           nc_px_backend_register(nc_pixel_backend_t *be);
esp_err_t           nc_px_backend_unregister(const char *name);
nc_pixel_backend_t *nc_px_backend_find(const char *name);
int                 nc_px_backend_count(void);

// "mock"：什麼都不碰，只計數。永遠可用（無硬件時驗整條路）。
nc_pixel_backend_t *nc_px_backend_mock(void);
void nc_px_backend_mock_bind(nc_pixel_backend_t *b, nc_pixel_q_t *q);

// "direct"：software relay。編碼後放進自己的 frame buffer 並計「已送」；
// 另一個消費者可以讀走（nc_px_direct_frame）。沒有 RMT 之前的過渡、也是 host 測試用。
nc_pixel_backend_t *nc_px_backend_direct(void);
void nc_px_backend_direct_bind(nc_pixel_backend_t *b, nc_pixel_q_t *q);
void nc_px_direct_bind(const nc_px_codec_cfg_t *codec_cfg);
bool nc_px_direct_frame(const uint8_t **units, uint32_t *len, uint32_t *seq);

// "i80"：esp_lcd I80（LCD_CAM 8080 並口）當 lanes 條 lane 的 DMA 動機。
//   pins = D0..D7、WR、DC（**固定 10 個**，跟 Kconfig 的字串同一個次序）；
//   前 lanes 個 D 是真 lane，其餘 D 是 dummy（I80 要求 8 條資料線都是有效 GPIO）。
typedef struct {
    // pins = D0..D(width-1)、WR、DC（width=8 → 10 個；width=16 → 18 個）
    const int     *pins;
    uint8_t        lanes;     // 1..width：前幾個 D 真的接燈
    uint8_t        bus_width; // 8 或 16（16 一個 PCLK 出兩條 lane 組，時間同 8-bit）
    uint32_t       pclk_hz;
    uint32_t       px;
    nc_px_format_t fmt;
    nc_px_wave_cfg_t wave;
    uint16_t       dither_bits;
    uint8_t        dither_mode;
    uint8_t        zone_px;
    uint8_t        slots;
} nc_px_i80_cfg_t;
nc_pixel_backend_t *nc_px_backend_i80(const nc_px_i80_cfg_t *cfg);

// "spi"：SPI MOSI/SCLK（WS2801／APA102）。一條 SPI 只有一個 MOSI。
typedef struct {
    int      pin_mosi;
    int      pin_sclk;
    int      host;          // SPI2_HOST / SPI3_HOST（-1 = 自動）
    uint32_t clock_hz;
    uint32_t px;
    nc_px_format_t fmt;
    uint16_t dither_bits;
    uint8_t  dither_mode;
    uint8_t  zone_px;
    uint8_t  bright;
    uint8_t  slots;
} nc_px_spi_cfg_t;
nc_pixel_backend_t *nc_px_backend_spi(const nc_px_spi_cfg_t *cfg);

// ═══════════════════════ 渲染器 ═══════════════════════
// 一個任務（名 "render"），跑一或多道 lane；每道 lane 一個後端 + 一條環。
typedef struct {
    char be[NC_PX_BACKEND_MAX];    // 後端名
    char ring[NC_PX_NAME_MAX + 1]; // 環名（空 = "<後端>.<lane>"）
} nc_px_lane_cfg_t;

typedef struct {
    uint16_t fps;             // 渲染節拍（0 = 盡快）
    int      nlane;
    nc_px_lane_cfg_t lane[NC_PX_MAX_LANES];
} nc_pixel_cfg_t;

void                  nc_render_set_cfg(const nc_pixel_cfg_t *cfg);
const nc_pixel_cfg_t *nc_render_cfg(void);

// ── 渲染器狀態（0xF061 的 "render" 段）──
typedef struct {
    uint32_t frames;      // 交出的幀（所有 lane）
    uint32_t fresh;       // 用到新格
    uint32_t starved;     // 後端全部在飛／沒有新格
    uint32_t late;        // 節拍遲到
    uint32_t fail;        // 編碼／後端失敗
    uint32_t skipped;     // 假消費掉的中間幀
    uint32_t drop;        // 環滿（生產者側）
    uint32_t frame_us;    // 最近一幀週期
    uint32_t frame_max_us;
    uint32_t enc_us;
    uint32_t tx_us;
    uint32_t fps_x10;
} nc_px_stat_t;

void   nc_render_get_stats(nc_px_stat_t *out);
size_t nc_render_json(char *out, size_t cap);     // 0xF061 與 STATUS 用

// 任務宣告（main.c 在 CONFIG_NETCORE_PIXEL 之下登記；未開回 NULL）
const nc_task_t *nc_render_task(void);

// 第一條真正綁到後端嘅環名（例如 i80 後端起唔到時會係 "mock.prd"）。
// 生產者（pair／pixel）唔應該靠「名入面有 prd」去猜自己餵邊條環。
const char *nc_render_first_ring(void);

#ifdef __cplusplus
}
#endif
