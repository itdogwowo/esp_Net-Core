// nc_pixel.h — firmware 側的完整面：**核心本身就是它**（環 + 編碼器，來自
// nc_pixel_core.h，firmware 與 hosttest 共用同一份），再加上：
//   ④ 後端驅動（i80／spi／direct／mock）：自己決定怎麼對齊、怎麼讀
//   ⑤ 渲染器設定與任務宣告
//
// 生產者（內建效果／`.nce`／主機推送）只需要 nc_pixel_core.h 那一段：
// canonical R,G,B,W × 4 B，交給一條環。
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
// 渲染器永遠不知道驅動長什麼樣 —— 新增一種驅動 = 新增一個後端，
// 環、節奏、假消費那一段完全不用改。
typedef struct nc_pixel_backend nc_pixel_backend_t;
struct nc_pixel_backend {
    char  name[NC_PX_BACKEND_MAX];   // "i80" / "spi" / "direct" / "mock"
    void *ctx;
    uint8_t lanes;                   // 這個後端幾條 lane（i80 可以 1..8）

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
//   pins = D0..D(lanes-1)、WR、DC（長度 lanes + 2）；沒用到的資料線自動用 D0 代替。
typedef struct {
    const int     *pins;
    uint8_t        lanes;
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
// 一個任務（名 "pixel"），跑一或多道 lane；每道 lane 一個後端 + 一條環。
typedef struct {
    char be[NC_PX_BACKEND_MAX];    // 後端名
    char ring[NC_PX_NAME_MAX + 1]; // 環名（空 = 由後端自己決定 = "<後端>.<lane>"）
} nc_px_lane_cfg_t;

typedef struct {
    uint16_t fps;             // 目標幀率（0 = 盡快）
    int      nlane;
    nc_px_lane_cfg_t lane[NC_PX_MAX_LANES];
} nc_pixel_cfg_t;

void                  nc_pixel_set_cfg(const nc_pixel_cfg_t *cfg);
const nc_pixel_cfg_t *nc_pixel_cfg(void);
// "i80,spi,direct,mock" → cfg.lane[]
int                   nc_px_cfg_parse_backends(nc_pixel_cfg_t *cfg, const char *list);

// 後端在 declare 之前要回答「一顆幾多」：0 = 這塊板沒有這個後端
uint32_t    nc_px_stage_px(const char *be);
uint32_t    nc_px_stage_slot_bytes(const char *be);
const char *nc_px_stage_ring(const char *be, int lane);

// 環註冊表（on_start 內；名字全系統唯一）
esp_err_t     nc_px_ring_register(nc_pixel_q_t *q);
nc_pixel_q_t *nc_px_ring_find(const char *name);
nc_pixel_q_t *nc_px_ring_at(int i);
int           nc_px_ring_count(void);
nc_pixel_q_t *nc_px_active_ring(void);
bool          nc_px_put(const uint8_t *rgbw, uint8_t src, uint16_t gen);

// 後端建立與綁定（on_start 內）
esp_err_t nc_px_setup_backends(void);
esp_err_t nc_px_bind_lanes(void);

// ── 渲染器狀態 ──
typedef struct {
    uint32_t frames;      // 交出的幀（所有 lane）
    uint32_t fresh;       // 用到新格
    uint32_t holds;       // 重播上一格（生產者沒更新）
    uint32_t starved;     // 後端全部在飛（DMA 未回）
    uint32_t late;        // 節拍遲到
    uint32_t fail;        // 編碼／後端失敗
    uint32_t skipped;     // 假消費掉的中間幀
    uint32_t drop;        // 環滿（生產者側）
    uint32_t frame_us;    // 最近一幀週期
    uint32_t frame_max_us;
    uint32_t enc_us;
    uint32_t tx_us;
    uint32_t fps_x10;
    // 內建效果生產者（"rgbmatrix"）—— 環的寫入端
    uint32_t prd_frames;  // 算過並提交幾格
    uint32_t prd_busy;    // 環滿而跳過幾拍（不覆蓋）
    uint32_t prd_fps_x10; // 生產者實測更新率
} nc_px_stat_t;

void   nc_px_get_stats(nc_px_stat_t *out);
size_t nc_px_json(char *out, size_t cap);     // 0xF061 與 STATUS 用

// 任務宣告（main.c 在 CONFIG_NETCORE_PIXEL 之下登記；未開回 NULL）
const nc_task_t *nc_pixel_task(void);

#ifdef __cplusplus
}
#endif
