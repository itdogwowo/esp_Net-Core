// 純 C 核心的公開面（環 + 編碼器）。**這是 ABI 的 bulk**：改欄位順序／大小就是改 ABI。
//
// 誰直接用它：
//   · firmware：nc_pixel.h 會 include 它，再補上後端／任務那一段；
//   · hosttest：tools/hosttest/pixel_test 直接編 nc_pixel_q.c / nc_pixel_encode.c，
//     用假時間與假 DMA 驗「假消費、DMA 還格、位元對應」。
// 兩邊是同一份實作、同一份長度 —— 不是兩份互相對照的程式。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── 幀格式（唯一，冇變體）──
#define NC_PX_BPP          4        // R,G,B,W（每像素 4 B）
#define NC_PX_HDR_BYTES    24       // 每格表頭
#define NC_PX_PAGE_BYTES   64       // 一格放大到 64 的倍數（payload 起點 = 64）
// 一格幾顆由設定（CONFIG_NETCORE_PIXEL_LEDS）決定，唔再用固定上限；
// 記憶體夠唔夠由實際配置（nc_buf／heap）回答，唔係由常數擋。
#define NC_PX_MAX_SLOTS    6        // 一條環最多幾格
#define NC_PX_NAME_MAX     24       // 環名上限（plan 資源名一律 16 字內，這裡寬鬆一點）

#define NC_PX_F_CLEAR      (1u << 0)   // 這格是全黑（熄燈／暫停的最後一格）
#define NC_PX_F_STILL      (1u << 1)   // 生產者不打算更新（靜態圖）

#define NC_PX_SRC_NONE     0
#define NC_PX_SRC_BUILTIN  1           // 韌體內建效果
#define NC_PX_SRC_NCM      2           // `.nce` 外掛效果
#define NC_PX_SRC_HOST     3           // 主機推送

// 一格 / 一條環幾個 B（plan 宣告與測試共用；兩邊一定算得一樣）
static inline uint32_t nc_px_payload_off(void) { return NC_PX_PAGE_BYTES; }

static inline uint32_t nc_px_slot_bytes(uint32_t px)
{
    const uint32_t used = nc_px_payload_off() + px * NC_PX_BPP;
    return (used + NC_PX_PAGE_BYTES - 1u) & ~(NC_PX_PAGE_BYTES - 1u);
}

static inline uint32_t nc_px_ring_bytes(uint32_t px, uint32_t n)
{
    return nc_px_slot_bytes(px) * n;
}

// ── 元資料通道：24 B，little-endian ──
// 寫入順序：先填所有欄位與 payload，**最後**才寫 seq（seq 是發布旗標）。
//   seq      生產者單調遞增（自 1）；重開機才歸零。0 = 未提交。
//   ts_us    這一格**內容**的時間戳（µs）。內容不變就沿用上一個值。
//   gen      換場代數（`.nce` 換模組、改效果…）。+1 = 整格重載。
//   src/flags  只給狀態顯示用。
//   drops    生產者因為環滿丟掉的幀數（給帳面）。
typedef struct {
    uint32_t seq;
    uint32_t ts_us;
    uint16_t gen;
    uint8_t  src;
    uint8_t  flags;
    uint32_t drops;
    uint8_t  _rsv[8];
} nc_px_hdr_t;

_Static_assert(sizeof(nc_px_hdr_t) == NC_PX_HDR_BYTES, "nc_px_hdr_t 必須 24 B（ABI）");

// ── 環儲存 ──
// 公開大小（firmware 用 static 陣列、測試用 auto 變數）：環本身**不配置記憶體**，
// 只是把管理器的區域切格。所以這個 struct 是「純 Data」，可以直接放 stack。
#define NC_PX_RING_STORAGE_BYTES  512

typedef struct nc_pixel_q {
    char             name[NC_PX_NAME_MAX + 1];
    uint8_t         *region;
    uint32_t         region_bytes;
    uint32_t         px;
    uint32_t         slots;
    uint32_t         slot_bytes;
    _Atomic uint8_t  state[NC_PX_MAX_SLOTS];   // FREE / FILLING / READY
    _Atomic uint32_t wr;                       // writer：下一個要寫的格
    _Atomic uint32_t seq;                      // 最新已提交的 seq（0 = 從來沒有）
    _Atomic uint32_t busy;                     // begin 與 commit/abort 之間
    uint32_t         puts;
    uint32_t         drops;
    uint32_t         overruns;
    uint32_t         stale;
} nc_pixel_q_t;

_Static_assert(sizeof(nc_pixel_q_t) <= NC_PX_RING_STORAGE_BYTES,
               "nc_pixel_q_t 超出 NC_PX_RING_STORAGE_BYTES（改常數要一齊改）");

typedef struct {
    const char *name;
    void       *region;      // 管理器交付的連續區域
    uint32_t    region_bytes;
    uint32_t    px;          // 每格幾顆（由設定決定）
    uint32_t    slots;       // 幾格（2..NC_PX_MAX_SLOTS）
} nc_px_ring_cfg_t;

nc_pixel_q_t *nc_px_ring_init(nc_pixel_q_t *q, const nc_px_ring_cfg_t *cfg);
void          nc_px_ring_deinit(nc_pixel_q_t *q);
uint8_t      *nc_px_ring_region(const nc_pixel_q_t *q);    // 第 0 格的起點（版面見下）
uint32_t      nc_px_ring_px(const nc_pixel_q_t *q);
uint32_t      nc_px_ring_slots(const nc_pixel_q_t *q);
uint32_t      nc_px_ring_slot_bytes(const nc_pixel_q_t *q);
uint32_t      nc_px_ring_fill(const nc_pixel_q_t *q);      // 已填、未取的格數
const char   *nc_px_ring_name(const nc_pixel_q_t *q);

// 像素編碼吞吐量測（internal-first；回傳統計 JSON）。release 都可以用（0xF062）。
size_t nc_px_bench_json(uint8_t lanes, uint16_t px, uint8_t frames, char *out, size_t cap);

// ── 環註冊表（跨任務用：renderer 建立／登記，生產者按名找回）──
// 回 0 = OK；-1 = 參數錯／重名／表滿。註冊表是全域的，但只有開機／on_start
// 期間會寫（之後只讀），所以不需要鎖。
int           nc_px_ring_register(nc_pixel_q_t *q);
nc_pixel_q_t *nc_px_ring_find(const char *name);
nc_pixel_q_t *nc_px_ring_at(int i);
int           nc_px_ring_count(void);
nc_pixel_q_t *nc_px_active_ring(void);     // 第 0 條（給只寫一條環的生產者）
bool          nc_px_put(const uint8_t *rgbw, uint8_t src, uint16_t gen);  // 寫 active ring

// ── 生產者（單一 writer）──
uint8_t *nc_px_ring_begin(nc_pixel_q_t *q);     // 滿了回 NULL（**不覆蓋**）
// 同上，另外交回「這一格」的表頭。生產者要寫 ts/gen/flags/drops 就必須用這個版本 ——
// 環的 writer 游標每 commit 一次就前進一格，所以「生產者綁死在第 0 格」是錯的：
// 要寫的格一定是 begin() 當下交出來的那一格，commit() 也只是發布那同一格。
uint8_t *nc_px_ring_begin_ex(nc_pixel_q_t *q, nc_px_hdr_t **hdr);
void     nc_px_ring_commit(nc_pixel_q_t *q);
void     nc_px_ring_abort(nc_pixel_q_t *q);
bool     nc_px_ring_put(nc_pixel_q_t *q, const uint8_t *rgbw, uint8_t src, uint16_t gen);
bool     nc_px_ring_put_ts(nc_pixel_q_t *q, const uint8_t *rgbw, uint8_t src, uint16_t gen,
                           uint32_t ts_us);

// ── 渲染器（單一 reader）──
// 取最新的格；回 payload 指標（R,G,B,W），NULL = 沒有新格。
//   生命週期：指標有效到下一次 commit 覆蓋同一格為止 —— 環是 SPSC 且格數 ≥ 2，
//   而後端一定在「同一輪」內編碼完。
const uint8_t *nc_px_ring_peek(nc_pixel_q_t *q, const nc_px_hdr_t **hdr, uint32_t *out_seq);
// 用完就還（seq = 剛才交出去的格）
void           nc_px_ring_release(nc_pixel_q_t *q, uint32_t seq);

// 取「下一個未取」的格：seq 最小而且 > after_seq 的那一格。**唔會假消費**其他格。
// 為什麼要有第二個取法：4 子幀時間抖動（12→10 bit）要四個子幀**全部**依次送出；
// peek 只會攞最新一格，其餘喺同一次 peek 就被釋放 → 抖動等於冇做。
// 所以多子幀行 FIFO（送唔切就背壓 paint），單子幀維持最新優先（舊幀冇意義）。
const uint8_t *nc_px_ring_next(nc_pixel_q_t *q, uint32_t after_seq,
                               const nc_px_hdr_t **hdr, uint32_t *out_seq);

// ── 取格政策（後端唔應該自己揀）──
// CONFIG_NETCORE_PIXEL_SUBFRAMES > 1 → 逐格 FIFO；否則 → 最新優先。
// host test 冇 sdkconfig，所以預設走 mailbox（同 firmware 單子幀一致）。
static inline bool nc_px_ring_ordered(void)
{
#if defined(CONFIG_NETCORE_PIXEL_SUBFRAMES) && (CONFIG_NETCORE_PIXEL_SUBFRAMES > 1)
    return true;
#else
    return false;
#endif
}

static inline const uint8_t *nc_px_ring_take(nc_pixel_q_t *q, uint32_t after_seq,
                                             const nc_px_hdr_t **hdr, uint32_t *out_seq)
{
    if (nc_px_ring_ordered()) {
        return nc_px_ring_next(q, after_seq, hdr, out_seq);
    }
    const uint8_t *p = nc_px_ring_peek(q, hdr, out_seq);
    if (p == NULL) {
        return NULL;
    }
    if (out_seq != NULL && *out_seq == after_seq) {
        return NULL;                       // 同一格已經交過
    }
    return p;
}

// 取到格但後端冇位放（全部 DMA 槽在飛）：
//   mailbox：一定要還格，否則同一格會一直係「最新」、阻住下一格。
//   FIFO：唔還 —— 下一輪 next() 會再攞返同一格，唔會漏子幀。
static inline void nc_px_ring_untake(nc_pixel_q_t *q, uint32_t seq)
{
    if (nc_px_ring_ordered()) {
        (void)q;
        (void)seq;
        return;
    }
    nc_px_ring_release(q, seq);
}

typedef struct {
    uint32_t puts;       // 成功交付
    uint32_t drops;      // 環滿而被拒
    uint32_t overruns;   // 同一個 writer 重入（錯誤用法，應永遠 0）
    uint32_t stale;      // peek 時沒有更新的格
} nc_px_q_stat_t;

void nc_px_ring_stats(const nc_pixel_q_t *q, nc_px_q_stat_t *out);

// ═══════════════════════ 編碼器 ═══════════════════════
// 「格式」= 一顆燈吃幾個位元組、次序、每個 bit 的物理樣本、reset。
// canonical → 格式 的轉換**只有這一份**，所以三個後端發出來的一定一模一樣。
typedef enum {
    NC_PX_FMT_WS2812 = 0,  // 24 bit 歸零碼，GRB（3 B/顆）
    NC_PX_FMT_WS2812W,     // 32 bit 歸零碼，GRBW（4 B/顆；WS2814/SK6812 同族）
    NC_PX_FMT_WS2815,      // 24 bit 歸零碼，RESET ≥ 280 µs
    NC_PX_FMT_WS2801,      // SPI 時脈（MSB first）。**沒有白色通道**
    NC_PX_FMT_APA102,      // SPI 時脈，亮度頭 + BGR
    NC_PX_FMT_COUNT,
} nc_px_format_t;

typedef struct {
    bool     clk;               // 有獨立時脈線（SPI）
    bool     bit_latch;         // SPI：非活動時資料腳拉低（APA102）
    uint8_t  head_bytes;        // SPI 每幀前面的 0x00 個數（APA102 = 4）
    uint8_t  tail_bytes;        // SPI 尾部 0x00 個數（APA102 = 4）
    uint8_t  lane_bytes;        // 3 或 4
    uint8_t  order[5];          // index = 通道（0=R 1=G 2=B 3=W 4=亮度頭）→ 線材位置
    uint32_t reset_us_hint;     // 建議 reset（歸零碼：拉低時間；SPI：hold low）
} nc_px_formatspec_t;

const nc_px_formatspec_t *nc_px_format_spec(nc_px_format_t f);
const char *nc_px_format_name(nc_px_format_t f);
bool nc_px_format_parse(const char *s, nc_px_format_t *out);

typedef struct {
    uint32_t clk_hz;
    uint32_t t0h;
    uint32_t t1h;
    uint32_t bit_cells;
} nc_px_wave_cfg_t;

void nc_px_wave_default(nc_px_format_t f, nc_px_wave_cfg_t *out);

typedef struct {
    nc_px_format_t    fmt;
    nc_px_wave_cfg_t  wave;
    uint8_t           bits;         // 抖動精度 8..16
    uint8_t           dither_mode;  // 0 off / 1 temporal / 2 spatial
    uint16_t          zone_px;      // 每幾顆一個相位區（0 = 整條一區）
    uint32_t          px;
    uint8_t           bright;       // APA102 亮度頭
} nc_px_codec_cfg_t;

typedef struct nc_pixel_codec nc_pixel_codec_t;

#define NC_PX_CODEC_ZONES_MAX   16
#define NC_PX_UNITS_PER_CELL    8
// 一幀 bytes 的上限 = 實際配置出來的槽大小（見 i80 的 slot_bytes），唔再設常數。

nc_pixel_codec_t *nc_px_codec_new(void);      // 配置（firmware 後端用；大小對呼叫端不透明）
nc_pixel_codec_t *nc_px_codec_init(nc_pixel_codec_t *c, const nc_px_codec_cfg_t *cfg);
void              nc_px_codec_free(nc_pixel_codec_t *c);
const nc_px_formatspec_t *nc_px_codec_spec(const nc_pixel_codec_t *c);
uint32_t nc_px_codec_lane_bytes(const nc_pixel_codec_t *c);
uint32_t nc_px_codec_bits(const nc_pixel_codec_t *c);          // 一幀幾個位（含頭尾）
uint32_t nc_px_codec_units(const nc_pixel_codec_t *c);         // 一幀幾個時序單元（不含 reset）
uint32_t nc_px_codec_total_units(const nc_pixel_codec_t *c);   // 含 reset
void     nc_px_codec_frame(nc_pixel_codec_t *c, uint32_t frame_idx);

// 一顆：canonical R,G,B,W → 線材位元組（含抖動與色序）
uint32_t nc_px_codec_lane_px(nc_pixel_codec_t *c, const uint8_t *rgbw, uint32_t zone,
                             uint8_t *lane_out);
// 歸零碼整幀：一顆一顆「先編碼、再快取、再展開」，reset 一併補 0
uint32_t nc_px_codec_wave_frame(nc_pixel_codec_t *c, const uint8_t *rgbw, uint32_t px,
                                uint8_t *out, uint32_t out_cap);
// 多 lane 專用：唔經 scratch，直接把 lane 的波形 OR 進 I80 byte。
//   out 必須先清零；每個單元寫 out[u*stride+off] |= (單元 << shift)。
//   8-bit bus：stride=1、off=0；16-bit bus：lane0..7 off=0、lane8..15 off=1、stride=2。
//   回傳一幀幾個單元（含 reset）；0 = 參數錯。
uint32_t nc_px_codec_wave_frame_or(nc_pixel_codec_t *c, const uint8_t *rgbw, uint32_t px,
                                   uint8_t *out, uint32_t cap,
                                   uint32_t stride, uint32_t off, uint8_t shift);

// packed 波形（LSB-first）：一條 lane 的波形壓成 bitstream（每 bit 一個時序單元），
// 回傳 bytes；配合 nc_px_codec_pack_merge8() 做 8×8 bit 轉置，輸出 I80 byte-per-unit。
uint32_t nc_px_codec_wave_pack(nc_pixel_codec_t *c, const uint8_t *rgbw, uint32_t px,
                               uint8_t *out, uint32_t cap);
// 把 8 條 lane 的 packed 波形合成 I80：out[(u)*stride+off] = bit k ← lane k 的第 u 個單元。
// 一次性 8 個單元（一個 byte）逐 bit 展開；out 必須先清零（reset 由呼叫端補 0）。
void nc_px_codec_pack_merge8(const uint8_t *lane[8], uint32_t packed_bytes,
                             uint8_t *out, uint32_t stride, uint32_t off);
// 同上，但用 store 覆蓋（唔 OR）：第一組 lane 用這個，之後的組仍然用 OR。
// 咁樣就唔需要每幀先 memset 整格（16×600 省 ~2–3.6 ms）；16-bit 第二組要用 OR。
void nc_px_codec_pack_merge8_store(const uint8_t *lane[8], uint32_t packed_bytes,
                                   uint8_t *out, uint32_t stride, uint32_t off);

// 一次過「展開＋轉置」：唔經每條 lane 的 packed 中間緩衝（wave_pack → merge8 的路徑）。
// 對每顆、每個線材 byte 即時由 lanes 個 codec 取位元組，做 8×8 bit 轉置，然後逐個
// tick 直接寫 bus word（8-bit：1 B；16-bit：2 B little-endian，bit k = lane k）。
// 呼叫前要對每條 lane 做 nc_px_codec_frame(c, seq)；out 只寫 active ticks，
// reset 部份由呼叫端保持 0。回傳寫出的 tick 數（px × lane_bytes × 8 × bit_cells）。
uint32_t nc_px_codec_pack_bus(nc_pixel_codec_t *const codec[], const uint8_t *rgbw,
                              uint32_t px, uint8_t *out, uint32_t lanes, uint32_t bus_width);

// SPI 整幀（頭部 + 每顆 + 尾巴）：out 每個 byte 一個位（bit0 = MOSI）
uint32_t nc_px_codec_spi_frame(nc_pixel_codec_t *c, const uint8_t *rgbw, uint32_t px,
                               uint8_t *out, uint32_t out_cap);

#ifdef __cplusplus
}
#endif
