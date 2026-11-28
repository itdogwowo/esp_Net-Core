// nc_px_paint_task.c — 四層管線第二段 pixel_paint_task（抖動＋配對＋上色）。
//
// 2026-11-25 四層四任務：
//   pixel_effect_task（值流）→ **pixel_paint_task**（本檔）→ pixel_encode_task（編碼）→
//   render_task（公用推送）。
//
// 本檔只做：
//   · /data/pixel 四層 → 建 plan／layout；
//   · 每幀拿 effect 最新值流 → 按 group/range/write 配對（抖動在內圈：apply_dither／apply4）
//     → white mix → 寫 prd 環；0xF06B／0xF06D／0xF06F 指令。
// 唔 encode、唔 submit、唔碰硬件；環由 render_task（公用輸出層）建立／登記。
//
// 為什麼獨立 component：四層資料係「用戶內容」（Web UI 上傳、可改、可換 mode），
// 生命週期同內建效果唔同；用 CONFIG_NETCORE_CONTENT 開關，release 映像可以有、
// 其他 project 完全唔編（同 effect_bench 一樣）。
//
// 資料流（每幀，零拷貝；舊的 canvas＋memset 路徑已退役）：
//   nc_px_ring_begin_ex() 拿環格 payload（4 B/px，R,G,B,W）
//   → nc_px_layout_apply*() 按 layout 逐 run 直接寫該格（r/g/b/w 只寫自己通道，
//     多個 run 疊在同一顆燈上就係「三個色通道分開寫」）
//   → white mix（mode 的 "white" 0..100，out=(out*(100-w)+255*w)/100）→ commit → step++
// 注意：layout 只寫它覆蓋到的通道，所以一個 mode 最好寫齊 R/G/B（或用 rgb 一次寫），
// 否則切 mode 之後冇被寫的通道會留住上一個 mode 的值。
// 環係 render_task 建立／登記的；這裡只負責寫。
//
// 失敗策略（唔好令 boot loop）：
//   /data 未掛載、/data/pixel 缺失、effects／mode／map 解析失敗、plan 冇 job、canvas
//   配置失敗 → log 清楚原因，進入 fallback：loop 仍然每幀填黑（輸出層會顯示黑），
//   唔會 crash、唔會卡住、唔會阻止開機。之後修好檔案重開機就會用真 plan。
//
// 0xF06B SHOWFRAME（回 0xF06C）：把最近一幀以 hex 交主機（Web UI poll）。
//   回覆 JSON：{"px":N,"seq":M,"hex":"<N*4 bytes 的 hex>"}；64 px = 512 hex 字元。
//   上限：px > 512 → {"err":"big"}（4 B/px 的 hex 太長）。
//   decode task 的棧很細（5 KB），所以 hex 直接寫入 decoder 的回覆幀緩衝，
//   唔喺棧上開大 buffer。
#include "sdkconfig.h"

#if CONFIG_NETCORE_CONTENT

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "nc4.h"
#include "nc_action.h"
#include "nc_pixel_core.h"
#include "nc_prio.h"
#include "nc_px_asset.h"
#include "nc_px_layer.h"
#include "nc_px_layer_scan.h"
#include "nc_px_layout.h"
#include "nc_px_show.h"
#include "nc_render.h"
#include "nc_pipeline.h"
#include "nc_store.h"
#include "nc_task.h"

static const char *TAG = "px_paint";

// ── 0xF06B／0xF06C：最近一幀 ──
#define NC4_CMD_SHOWFRAME     0xF06B
#define NC4_CMD_SHOWFRAME_RSP 0xF06C

// 快照上限：>512 px 唔回（4 B/px → 4096 hex 字元，遠超 decoder 回覆上限）。
// 另有一層回覆預算檢查（NC4_REPLY_JSON_MAX）：真正可以回幾多 px 由它以 sizeof 為準。
#define SHOWFRAME_PX_MAX 512

// ═══════════════════════ 狀態 ═══════════════════════
// plan（~4.4 KB）、asset 表（~8.4 KB）都放 static：唔可以放棧（on_start 由任務管理器
// 的 boot 路徑呼叫）。canvas 按環的 px 用 heap 配（8×8 = 256 B）。
typedef struct {
    bool           ready;       // plan 建好、可以逐幀合成
    bool           fallback;    // 載入／plan 失敗 → 每幀填黑
    char           fallback_why[56];  // 0xF06B 用：點解 fallback（維運用）
    nc_pixel_q_t  *q;           // render 建立的 prd 環（NULL = render 未起）
    uint32_t       px;          // 環的 px（= inventory 的 px）
    uint8_t       *canvas;      // px × 4（固定 PSRAM）；fallback/未 ready 時可為 NULL
    uint32_t       step;        // 逐幀遞增的全域步數（動畫時間）
    uint8_t        white;       // 0..100；mode JSON 的 white／white_mix
    uint32_t       commits;     // 成功提交的幀數（= 0xF06B 的 seq）
    uint32_t       busy;        // 環滿而跳過的次數（自報 drops）

    nc_px_layout_t    layout;   // 排版編譯結果（dst 順序 runs；由 nc_px_layout 擁有）
    nc_px_asset_tbl_t *assets;  // PSRAM：只收 mode 引用到的 asset（~8.4 KB，唔佔 internal）

    // 下一張（playlist 預測）：layout＋flat 預建好，切換時 swap（0 ms）
    nc_px_layout_t     next_layout;
    nc_px_asset_tbl_t *next_assets;
    char               next_mode_name[NC_PX_LAYER_NAME_MAX];
    char               next_map_name[NC_PX_LAYER_NAME_MAX];
    bool               next_ready;
    uint32_t           next_us;
    uint8_t            next_why;      // 診斷：0=未試、1=ok、2=冇下一個、3=asset、4=map、5=build、6=no_ready
    char              mode_name[NC_PX_LAYER_NAME_MAX];
    char              map_name[NC_PX_LAYER_NAME_MAX];
    char              pixel_dir[64];      // /data/pixel（reload 用）
    bool              auto_play;          // registry.json 的 auto_play
    uint32_t          mode_frames;        // 這個 mode 已播幾幀
    uint32_t          mode_limit;         // 幾幀後換下一個（maxF，0=500）

    // 0xF06B 快照：最近一次 commit 的 4 B/px 畫面（最多 SHOWFRAME_PX_MAX 顆）。
    // 由 paint loop 寫、nc4 decode task 讀 → 用 spinlock 護住。
    portMUX_TYPE  lock;
    uint8_t      *last;         // PSRAM：0xF06B 快照（SHOWFRAME_PX_MAX × 4 B）
    uint32_t      last_px;
    uint32_t      last_seq;
    bool          last_valid;
} show_state_t;

static show_state_t s_content = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
};

// 0xF06D：decode task 只排隊，真正 rebuild plan 由 paint loop 喺幀邊界做
// （plan／asset table 係 content task 擁有，唔可以同 decode task 同時改）。
static volatile uint8_t s_pending_mode = 0xFF;
static volatile bool    s_pending_reload;
static uint32_t s_last_apply_us;

// pair 擁有的 layout；effect 只經 nc_pipe_aprog_publish()／snapshot 拿 aprog 副本。
static SemaphoreHandle_t s_plan_mtx;
static volatile uint32_t s_plan_gen;
static uint32_t s_last_eff_seq;
// 多子幀：同一個視覺幀可能交到一半就環滿。記住交到邊個子幀，下一輪由嗰度續，
// 否則重試會由 sf=0 再交一次（同一個子幀重覆入環，抖動相位就亂）。
static uint32_t s_sf_seq;                  // s_sf_done 屬於邊個 effect seq
static uint32_t s_sf_done;                 // 已經 commit 咗幾個子幀
static nc_pipe_effect_frame_t s_eff_frame;

// 找回「效果用」的環：先問 render 實際綁到邊條（i80 後端起唔到時會係 mock.prd），
// 問唔到才退回「名入面有 prd」的舊規則。
static nc_pixel_q_t *find_prd_ring(void)
{
    const char *want = nc_render_first_ring();
    if (want != NULL) {
        for (int i = 0; i < nc_px_ring_count(); i++) {
            nc_pixel_q_t *q = nc_px_ring_at(i);
            const char *nm = nc_px_ring_name(q);
            if (nm != NULL && strcmp(nm, want) == 0) {
                return q;
            }
        }
    }
    for (int i = 0; i < nc_px_ring_count(); i++) {
        nc_pixel_q_t *q = nc_px_ring_at(i);
        const char *nm = nc_px_ring_name(q);
        if (nm != NULL && strstr(nm, "prd") != NULL) {
            return q;
        }
    }
    return NULL;
}

// ═══════════════════════ 逐幀 ═══════════════════════

// white mix：四個通道都做（R,G,B,W），整數除法。
// w=0 → 完全唔混（走快路）；w=100 → 四通道全 255。
static void white_mix(uint8_t *canvas, uint32_t px, uint8_t w)
{
    if (w == 0 || canvas == NULL) {
        return;
    }
    const uint32_t a = 100u - (uint32_t)w;
    const uint32_t n = px * NC_PX_BPP;
    for (uint32_t i = 0; i < n; i++) {
        canvas[i] = (uint8_t)(((uint32_t)canvas[i] * a + 255u * (uint32_t)w) / 100u);
    }
}

// 效果表的指紋：layout／prog 都係由 effects.json 編譯出來，但 cache key 原本只計
// mode＋map＋px → 改 effects.json 之後 key 唔變，會讀回舊 program（燈唔動／反應唔到）。
// 這裡把整個效果表（名＋每段欄位＋播放參數）滾成一個 32-bit 指紋，餵入 cache key。
static uint32_t assets_hash(const nc_px_asset_tbl_t *t)
{
    uint32_t h = 2166136261u;
    if (t == NULL) {
        return h;
    }
    for (uint16_t i = 0; i < t->n; i++) {
        const nc_px_asset_t *a = &t->item[i];
        for (const char *c = a->name; *c != '\0'; c++) {
            h = (h ^ (uint8_t)*c) * 16777619u;
        }
        const uint16_t small[6] = { a->nseg, a->step, a->spacing, a->offset, a->speed,
                                    (uint16_t)(a->reverse ? 1u : 0u) };
        for (int k = 0; k < 6; k++) {
            const uint8_t *b = (const uint8_t *)&small[k];
            h = (h ^ b[0]) * 16777619u;
            h = (h ^ b[1]) * 16777619u;
        }
        for (uint16_t g = 0; g < a->nseg; g++) {
            const nc_px_seg_t *sg = nc_px_asset_seg(a, g);
            if (sg == NULL) {
                break;
            }
            const uint16_t seg[7] = { sg->type, sg->f, sg->l_max,
                                      sg->l_lim, sg->phi, sg->end_f, sg->pulse };
            for (int k = 0; k < 7; k++) {
                const uint8_t *b = (const uint8_t *)&seg[k];
                h = (h ^ b[0]) * 16777619u;
                h = (h ^ b[1]) * 16777619u;
            }
        }
    }
    return h;
}

// 最新一幀的快照：paint loop 寫、0xF06B handler 讀。
// rgbw = NULL 代表「黑幀」（fallback 時用），會把快照內的有效像素清 0。
static struct {
    uint32_t present, built, too_big, failed, ms;
} s_prebuild;
static void cache_prebuild_missing(void);

// ── NCLAYOUT 快取預算／LRU ──
// 純快取，刪咗只係下次切 mode 重建；用 mtime 揀最舊，避免 mode／map 多時食滿 FAT。
#define LAYOUT_CACHE_SUFFIX ".layout"
#define LAYOUT_CACHE_FILE_OVERHEAD 128u

static uint32_t layout_cache_budget(void)
{
#if defined(CONFIG_NETCORE_PIXEL_LAYOUT_CACHE_KB) && CONFIG_NETCORE_PIXEL_LAYOUT_CACHE_KB > 0
    return (uint32_t)CONFIG_NETCORE_PIXEL_LAYOUT_CACHE_KB * 1024u;
#else
    return 0u;                                  // 0 = 不限
#endif
}

// 掃 cache 目錄：files／bytes；oldest 回最舊的檔名（可選，skip 傳 NULL）。
static void layout_cache_scan(uint32_t *files, uint32_t *bytes,
                              char *oldest, size_t oldest_cap, const char *skip)
{
    if (files != NULL) *files = 0;
    if (bytes != NULL) *bytes = 0;
    if (oldest != NULL && oldest_cap > 0) oldest[0] = '\0';
    time_t oldest_t = 0;
    char cdir[96];
    (void)snprintf(cdir, sizeof(cdir), "%s/cache", s_content.pixel_dir);
    DIR *d = opendir(cdir);
    if (d == NULL) {
        return;
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const size_t nl = strlen(de->d_name);
        const size_t sl = sizeof(LAYOUT_CACHE_SUFFIX) - 1;
        if (nl <= sl || strcmp(de->d_name + nl - sl, LAYOUT_CACHE_SUFFIX) != 0) {
            continue;
        }
        char path[192];
        (void)snprintf(path, sizeof(path), "%s/%s", cdir, de->d_name);
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        if (files != NULL) (*files)++;
        if (bytes != NULL) *bytes += (uint32_t)st.st_size;
        if (oldest != NULL && (oldest[0] == '\0' || st.st_mtime < oldest_t) &&
            (skip == NULL || strcmp(path, skip) != 0)) {
            oldest_t = st.st_mtime;
            (void)snprintf(oldest, oldest_cap, "%s", path);
        }
    }
    closedir(d);
}

static uint32_t s_cache_trimmed;

// 確保再加 need bytes 之後唔超預算；超就刪最舊（skip=剛要寫的那個檔）。
static bool layout_cache_trim(size_t need, const char *skip)
{
    const uint32_t budget = layout_cache_budget();
    if (budget == 0u) {
        return true;
    }
    for (;;) {
        uint32_t files = 0, bytes = 0;
        layout_cache_scan(&files, &bytes, NULL, 0, NULL);
        if ((size_t)bytes + need <= (size_t)budget) {
            return true;
        }
        char oldest[192];
        layout_cache_scan(NULL, NULL, oldest, sizeof(oldest), skip);
        if (oldest[0] == '\0') {
            return false;                       // 得返自己／冇檔可刪
        }
        if (unlink(oldest) == 0) {
            s_cache_trimmed++;
            ESP_LOGI(TAG, "快取超預算（%u + %u > %u）→ 刪最舊 %s",
                     (unsigned)bytes, (unsigned)need, (unsigned)budget, oldest);
        } else {
            ESP_LOGW(TAG, "快取超預算但刪唔到 %s（errno %d）", oldest, errno);
            return false;
        }
    }
}

// 存一張 layout cache；need 已含 header overhead。回 ESP_ERR_NO_MEM = 超預算唔存。
static esp_err_t layout_cache_save(const nc_px_layout_t *l, const char *path, uint32_t key,
                                   size_t payload_bytes)
{
    const size_t need = payload_bytes + LAYOUT_CACHE_FILE_OVERHEAD;
    if (!layout_cache_trim(need, path)) {
        ESP_LOGW(TAG, "layout 快取 %u B 超出預算 → 唔存（下次重建）", (unsigned)need);
        return ESP_ERR_NO_MEM;
    }
    char cdir[96];
    (void)snprintf(cdir, sizeof(cdir), "%s/cache", s_content.pixel_dir);
    (void)mkdir(cdir, 0777);                    // EEXIST 當成功
    return nc_px_layout_save(l, path, key);
}


#if CONFIG_NETCORE_PIXEL_APPLY_PIPE
#define PIPE_CHUNK_KB 16u
#define PIPE_BUFS     3u
typedef struct {
    const nc_px_layout_t *l;
    uint32_t nflat;
    uint32_t epc;
    uint32_t bytes;
    uint8_t *buf[PIPE_BUFS];
    SemaphoreHandle_t kick;
    SemaphoreHandle_t full[PIPE_BUFS];
    SemaphoreHandle_t free_s[PIPE_BUFS];
    volatile bool stop;
    bool ready;
} pipe_ctx_t;
static pipe_ctx_t s_pipe;
static volatile uint32_t s_pipe_ok, s_pipe_fail;
static bool px_pipe_apply(uint8_t *dst);
static void pipe_stop(void);
#endif

static volatile bool s_snap_wanted;   // 0xF06B 要求預覽時才由環複製一份

static void snapshot_publish(const uint8_t *rgbw, uint32_t px, uint32_t seq)
{
    const uint32_t n = (px > SHOWFRAME_PX_MAX) ? SHOWFRAME_PX_MAX : px;
    taskENTER_CRITICAL(&s_content.lock);
    if (n > 0 && s_content.last != NULL) {
        if (rgbw != NULL) {
            memcpy(s_content.last, rgbw, (size_t)n * NC_PX_BPP);
        } else {
            memset(s_content.last, 0, (size_t)n * NC_PX_BPP);
        }
    }
    s_content.last_px = px;
    s_content.last_seq = seq;
    s_content.last_valid = true;
    taskEXIT_CRITICAL(&s_content.lock);
}

// 把 canvas 交入環。回 false = 環滿（renderer 未還格）。
// 表頭：src = BUILTIN（畫面由韌體算，唔係主機 push）、ts = 算完的時間、
// drops = 自己因為環滿跳過的幀數（環的帳面會顯示）。
// 交棒：槽內容已經寫好，只補表頭＋commit（零拷貝）。
static bool ring_commit_slot(nc_px_hdr_t *hdr)
{
    if (s_content.q == NULL) {
        return false;
    }
    if (hdr != NULL) {
        hdr->ts_us = (uint32_t)esp_timer_get_time();
        hdr->gen = 0;
        hdr->src = NC_PX_SRC_BUILTIN;
        hdr->flags = 0;
        hdr->drops = s_content.busy;
    }
    nc_px_ring_commit(s_content.q);
    s_content.commits++;
    return true;
}

// 一幀：ready → 四層合成；唔 ready → 黑（fallback）。
static bool content_frame_once(void)
{
    if (s_content.q == NULL) {
        return false;
    }
    if (s_content.ready && s_content.layout.njobs > 0) {
        // pair 只讀 effect 的最新 fixed frame；effect 不會直接碰 canvas/ring。
        if (!nc_pipe_effect_copy(&s_eff_frame)) {
            return false;                 // effect 未出第一幀
        }
        if (s_eff_frame.plan_gen != s_plan_gen) {
            return false;                 // plan 剛換，effect 未趕上
        }
        if (s_eff_frame.seq == s_last_eff_seq) {
            return false;                 // 未有新 frame
        }
        const uint32_t lat_us = ((uint32_t)esp_timer_get_time() > s_eff_frame.t_pub_us)
                                ? (uint32_t)esp_timer_get_time() - s_eff_frame.t_pub_us : 0;
#if CONFIG_NETCORE_PIXEL_SUBFRAMES > 1
        const uint32_t ns = (uint32_t)CONFIG_NETCORE_PIXEL_SUBFRAMES;
        uint32_t apply_us = 0, white_us = 0, commit_us = 0, clear_us = 0;
        const int64_t t_all0 = esp_timer_get_time();
        if (s_eff_frame.seq != s_sf_seq) {
            s_sf_seq = s_eff_frame.seq;
            s_sf_done = 0;                 // 新視覺幀：由第一個子幀開始
        }
        for (uint32_t sf = s_sf_done; sf < ns; sf++) {
            nc_px_hdr_t *hp = NULL;
            uint8_t *sp = nc_px_ring_begin_ex(s_content.q, &hp);
            if (sp == NULL) {
                nc_pipe_stats_full();
                return false;             // 環滿：已 commit 的子幀照用，下一輪再試
            }
            // 唔使成格 clear：layout 的每個 write（r/g/b/w/ww/rgb/rgbw）現在都會
            // 完整寫齊 4 個通道（單通道會把其餘設 0），所以環格重用都唔會殘留。
            const int64_t tc0 = esp_timer_get_time();
            if (nc_px_layout_needs_clear(&s_content.layout)) {
                memset(sp, 0, (size_t)s_content.px * 4u);
            }
            const int64_t tc1 = esp_timer_get_time();
            clear_us += (uint32_t)(tc1 - tc0);
            const int64_t t0 = esp_timer_get_time();
            nc_px_layout_apply_dither(&s_content.layout,
                                      (uint16_t *const *)s_eff_frame.vals,
                                      sp, (uint8_t)ns, (uint8_t)sf, s_content.commits);
            const int64_t t1 = esp_timer_get_time();
            white_mix(sp, s_content.px, s_content.white);
            const int64_t t2 = esp_timer_get_time();
            if (!ring_commit_slot(hp)) {
                nc_pipe_stats_full();
                return false;
            }
            s_sf_done = sf + 1;             // 環滿重試時由呢度續，唔會重交
            const int64_t t3 = esp_timer_get_time();
            apply_us += (uint32_t)(t1 - t0);
            white_us += (uint32_t)(t2 - t1);
            commit_us += (uint32_t)(t3 - t2);
            if (sf == 0 && s_snap_wanted) {
                snapshot_publish(sp, s_content.px, s_content.commits);
                s_snap_wanted = false;
            }
        }
        (void)t_all0;
        s_sf_done = 0;                     // 四個子幀交齊
        nc_pipe_stats_pair(clear_us, apply_us, white_us, commit_us, lat_us);
#else
        nc_px_hdr_t *hdr = NULL;
        const int64_t a0 = esp_timer_get_time();
        uint8_t *p = nc_px_ring_begin_ex(s_content.q, &hdr);
        if (p == NULL) {
            nc_pipe_stats_full();
            return false;                 // 環滿：下 tick 再試
        }
        // write=r/g/b/w 或 flat 表未覆蓋全部 px 時，canvas 要由零開始；
        // 全部 RGB/RGBW 且 full coverage 就唔使清。
        const int64_t clr0 = esp_timer_get_time();
        if (nc_px_layout_needs_clear(&s_content.layout)) {
            memset(p, 0, (size_t)s_content.px * 4u);
        }
        const int64_t clr1 = esp_timer_get_time();
#if CONFIG_NETCORE_PIXEL_APPLY_PIPE
        s_pipe.l = &s_content.layout;
        s_pipe.nflat = s_content.layout.nflat;
        if (!px_pipe_apply(p))
#endif
        {
            nc_px_layout_apply(&s_content.layout, s_eff_frame.vals, p);
        }
        const int64_t a1 = esp_timer_get_time();
        white_mix(p, s_content.px, s_content.white);
        const int64_t w1 = esp_timer_get_time();
        if (!ring_commit_slot(hdr)) {
            nc_pipe_stats_full();
            return false;
        }
        const int64_t c1 = esp_timer_get_time();
        nc_pipe_stats_pair((uint32_t)(clr1 - clr0), (uint32_t)(a1 - a0), (uint32_t)(w1 - a1),
                           (uint32_t)(c1 - w1), lat_us);
        if (s_snap_wanted) {
            snapshot_publish(p, s_content.px, s_content.commits);
            s_snap_wanted = false;
        }
#endif
        s_last_eff_seq = s_eff_frame.seq;
        nc_pipe_effect_ack(s_eff_frame.seq);   // effect 可以重用該 slot
        return true;
    }
    // fallback：填黑。直接寫環的 payload（唔需要 canvas）。
    nc_px_hdr_t *hdr = NULL;
    uint8_t *p = nc_px_ring_begin_ex(s_content.q, &hdr);
    if (p == NULL) {
        s_content.busy++;
        return false;
    }
    memset(p, 0, (size_t)s_content.px * NC_PX_BPP);
    if (hdr != NULL) {
        hdr->ts_us = (uint32_t)esp_timer_get_time();
        hdr->gen = 0;
        hdr->src = NC_PX_SRC_BUILTIN;
        hdr->flags = NC_PX_F_CLEAR;
        hdr->drops = s_content.busy;
    }
    nc_px_ring_commit(s_content.q);
    s_content.commits++;
    snapshot_publish(NULL, s_content.px, s_content.commits);   // 快照 = 黑
    return true;
}

static esp_err_t content_apply_mode(const nc_px_mode_t *mode);static esp_err_t content_apply_mode(const nc_px_mode_t *mode);
static void content_prepare_next(void);
static esp_err_t content_swap_next(const nc_px_mode_t *mode);
static esp_err_t content_reload(void);
static void content_auto_advance(void);
static void content_set_fallback(const char *why);

static void content_loop(void *arg)
{
    (void)arg;
    while (!nc_task_exit_requested()) {
        if (s_content.q == NULL) {
            nc_task_beat();
            vTaskDelay(pdMS_TO_TICKS(100));      // render 未起：閒著等
            continue;
        }
        // 0xF06F 排隊 reload：重新掃 /data/pixel，再按目前 mode 名重建 plan。
        if (s_pending_reload) {
            s_pending_reload = false;
            (void)content_reload();
        }
        // 0xF06D 排隊：喺幀邊界 rebuild plan；唔會同 decode task 同時改 plan。
        const uint8_t pending = s_pending_mode;
        if (pending != 0xFF) {
            s_pending_mode = 0xFF;
            const nc_px_mode_tbl_t *tbl = nc_px_mode_table();
            const nc_px_mode_t *next = NULL;
            if (tbl != NULL) {
                for (uint8_t i = 0; i < tbl->n; i++) {
                    if (tbl->item[i].id == pending) {
                        next = &tbl->item[i];
                        break;
                    }
                }
            }
            if (next != NULL) {
                if (content_apply_mode(next) != ESP_OK) {
                    content_set_fallback("切換 mode 失敗");
                }
            }
        }
        const bool ok = content_frame_once();
        nc_task_beat();
        if (!ok) {
            // 環滿（renderer 由 frame_interval_ms 控制，通常 20 ms 一格）：
            // 1 tick 後再看有沒有還格。
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        content_auto_advance();                   // registry auto_play（如果 list>1）
        content_prepare_next();                   // 播放時預建下一張（playlist 次序）
        taskYIELD();                              // render（prio 更高）先跑
    }
}

// ═══════════════════════ 開機：四層 → plan ═══════════════════════

static void content_set_fallback(const char *why)
{
    s_content.ready = false;
    s_content.fallback = true;
    snprintf(s_content.fallback_why, sizeof(s_content.fallback_why), "%s",
             (why != NULL) ? why : "?");
    ESP_LOGW(TAG, "%s → fallback：每幀填黑（render 會顯示黑；開機照常）", why);
}

static void content_state_reset(void)
{
    if (s_content.canvas != NULL) {
        heap_caps_free(s_content.canvas);
        s_content.canvas = NULL;
    }
    s_content.ready = false;
    s_content.fallback = false;
    s_content.q = NULL;
    s_content.px = 0;
    s_content.step = 0;
    s_content.white = 0;
    s_content.commits = 0;
    s_content.busy = 0;
    nc_px_layout_free(&s_content.layout);
    nc_px_layout_free(&s_content.next_layout);
    s_content.next_ready = false;
    s_content.next_mode_name[0] = '\0';
    if (s_content.assets != NULL) {
        s_content.assets->n = 0;          // 保留已配置的 item 緩衝，下次換場重用
    }
    s_content.mode_name[0] = '\0';
    s_content.map_name[0] = '\0';
    s_content.pixel_dir[0] = '\0';
    s_plan_gen = 0;
    s_last_eff_seq = 0;
    s_sf_seq = 0;
    s_sf_done = 0;
    s_content.auto_play = false;
    s_content.mode_frames = 0;
    s_content.mode_limit = 500;
    s_content.last_valid = false;
    s_content.last_px = 0;
    s_content.last_seq = 0;
    s_content.fallback_why[0] = '\0';
}

// mode 的 mapping 欄 → map；空／搵唔到 → 用第一條 entry 的 group 前綴；
// 仍然搵唔到 → map 表第一個。全部搵唔到回 NULL。
static const nc_px_map_t *select_map(const nc_px_mode_t *mode)
{
    const nc_px_map_tbl_t *maps = nc_px_map_table();
    const nc_px_map_t *map = NULL;

    if (mode->mapping[0] != '\0') {
        map = nc_px_map_find(mode->mapping);
        if (map == NULL) {
            ESP_LOGW(TAG, "mode %s 的 mapping=\"%s\" 搵唔到", mode->name, mode->mapping);
        }
    }
    if (map == NULL && mode->nentry > 0) {
        // "matrix.full" → "matrix"（mode 冇寫 mapping 時仍然可以指定 map）
        const char *g = mode->entry[0].group;
        const char *dot = strchr(g, '.');
        if (dot != NULL && dot != g) {
            char ref[NC_PX_LAYER_NAME_MAX];
            const size_t n = (size_t)(dot - g);
            if (n < sizeof(ref)) {
                memcpy(ref, g, n);
                ref[n] = '\0';
                map = nc_px_map_find(ref);
            }
        }
    }
    if (map == NULL && maps != NULL && maps->n > 0) {
        map = &maps->item[0];
        ESP_LOGW(TAG, "唔到對應 map → 用 map 表第一個（%s）", map->name);
    }
    return map;
}

// plan 只需要 mode 引用到的 asset（最多 NC_PX_MODE_ENTRY_MAX 個）；
// asset 表本身係 sw/pixel 的私有表，只公開 find/at，所以在這裡建一份小表。
// 按 entry 數配置（distinct 效果一定 ≤ entry 數），所以冇固定上限，
// 亦唔會靜靜 drop 第 33 個效果。
static esp_err_t build_asset_tbl_into(nc_px_asset_tbl_t *dst, const nc_px_mode_t *mode)
{
    dst->n = 0;
    if (nc_px_asset_tbl_reserve(dst, (uint16_t)(mode->nentry ? mode->nentry : 1u)) != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    for (uint32_t i = 0; i < mode->nentry; i++) {
        const nc_px_asset_t *a = nc_px_asset_find(mode->entry[i].effect);
        if (a == NULL) {
            continue;                     // 未知 effect：plan 會記入 skipped
        }
        bool dup = false;
        for (uint16_t k = 0; k < dst->n; k++) {
            if (strcmp(dst->item[k].name, a->name) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }
        dst->item[dst->n++] = *a;
    }
    return ESP_OK;
}

#if CONFIG_NETCORE_PIXEL_APPLY_PIPE
// ── flat 表 chunk 預取：core0 worker 做 PSRAM→internal memcpy ──

static void pipe_worker(void *arg)
{
    pipe_ctx_t *c = (pipe_ctx_t *)arg;
    while (1) {
        if (xSemaphoreTake(c->kick, portMAX_DELAY) != pdTRUE) {
            break;
        }
        if (c->stop) {
            break;
        }
        const uint32_t nch = (c->nflat + c->epc - 1u) / c->epc;
        for (uint32_t i = 0; i < nch; i++) {
            const uint32_t slot = i % PIPE_BUFS;
            xSemaphoreTake(c->free_s[slot], portMAX_DELAY);
            uint32_t cnt = c->epc;
            if (i * c->epc + cnt > c->nflat) {
                cnt = c->nflat - i * c->epc;
            }
            memcpy(c->buf[slot], (const uint8_t *)c->l->flat + (size_t)i * c->epc * 6u,
                   (size_t)cnt * 6u);
            xSemaphoreGive(c->full[slot]);
        }
    }
    vTaskDelete(NULL);
}

static bool pipe_start(void)
{
    if (s_pipe.ready) {
        return true;
    }
    memset(&s_pipe, 0, sizeof(s_pipe));
    s_pipe.epc = (PIPE_CHUNK_KB * 1024u) / 6u;
    s_pipe.bytes = s_pipe.epc * 6u;
    s_pipe.kick = xSemaphoreCreateBinary();
    if (s_pipe.kick == NULL) {
        return false;
    }
    for (uint32_t i = 0; i < PIPE_BUFS; i++) {
        s_pipe.buf[i] = (uint8_t *)heap_caps_malloc(s_pipe.bytes,
                                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        s_pipe.full[i] = xSemaphoreCreateBinary();
        s_pipe.free_s[i] = xSemaphoreCreateBinary();
        if (s_pipe.buf[i] == NULL || s_pipe.full[i] == NULL || s_pipe.free_s[i] == NULL) {
            return false;
        }
        xSemaphoreGive(s_pipe.free_s[i]);
    }
    if (xTaskCreatePinnedToCore(pipe_worker, "pxpipe", 3072, &s_pipe, 2, NULL, 1) != pdPASS) {
        return false;
    }
    s_pipe.ready = true;
    ESP_LOGI(TAG, "apply pipe：%u KB × %u 塊（internal）、worker core0",
             (unsigned)PIPE_CHUNK_KB, (unsigned)PIPE_BUFS);
    return true;
}

static void pipe_stop(void)
{
    if (s_pipe.ready) {
        s_pipe.stop = true;
        xSemaphoreGive(s_pipe.kick);
        s_pipe.ready = false;
    }
}

// 用預取 pipeline 套用目前 layout；失敗就退回直接 apply。
static bool px_pipe_apply(uint8_t *dst)
{
    if (!s_pipe.ready || s_pipe.l == NULL || s_pipe.l->flat == NULL || s_pipe.nflat == 0) {
        return false;
    }
    xSemaphoreGive(s_pipe.kick);
    const uint32_t nch = (s_pipe.nflat + s_pipe.epc - 1u) / s_pipe.epc;
    for (uint32_t i = 0; i < nch; i++) {
        const uint32_t slot = i % PIPE_BUFS;
        if (xSemaphoreTake(s_pipe.full[slot], pdMS_TO_TICKS(50)) != pdTRUE) {
            s_pipe_fail++;
            return false;                  // worker 卡住：退回直接 apply
        }
        uint32_t cnt = s_pipe.epc;
        if (i * s_pipe.epc + cnt > s_pipe.nflat) {
            cnt = s_pipe.nflat - i * s_pipe.epc;
        }
        (void)nc_px_layout_apply_range(s_pipe.l, s_eff_frame.vals, dst, i * s_pipe.epc, cnt);
        xSemaphoreGive(s_pipe.free_s[slot]);
    }
    s_pipe_ok++;
    return true;
}
#endif

// 給目前的 mode 建表（s_content.assets 是這個 mode 專用的）
static esp_err_t build_asset_tbl(const nc_px_mode_t *mode)
{
    return build_asset_tbl_into(s_content.assets, mode);
}

static void log_tree(const char *pixel_dir)
{
    const nc_px_map_tbl_t *maps = nc_px_map_table();
    const nc_px_mode_tbl_t *modes = nc_px_mode_table();
    const nc_px_show_t *show = nc_px_show();
    ESP_LOGI(TAG, "pixel 樹：%s → effects=%d maps=%u modes=%u list=%u",
             pixel_dir, nc_px_asset_count(),
             (maps != NULL) ? (unsigned)maps->n : 0u,
             (modes != NULL) ? (unsigned)modes->n : 0u,
             (show != NULL) ? (unsigned)show->nlist : 0u);
}

// 上傳的前提：FILE_BEGIN 直接 fopen("<path>.part","wb")，唔會 mkdir 父目錄
// （file_action 的行為）。main.c 的 seed_dirs() 只建 /data/pixel 同 /pixel/effects，
// 所以第一次用 Web UI 上傳 map/、modes/ 會 err_write_fail。content 是這些目錄的唯一
// 消費者，就在 on_start 建好（等於 show 自己的初始化）。EEXIST 當成功；失敗只 warn。
static void ensure_pixel_dirs(const char *pixel_dir)
{
    char path[64 + 16];
    static const char *const sub[] = { "map", "modes" };
    for (size_t i = 0; i < sizeof(sub) / sizeof(sub[0]); i++) {
        const int n = snprintf(path, sizeof(path), "%s/%s", pixel_dir, sub[i]);
        if (n <= 0 || (size_t)n >= sizeof(path)) {
            continue;
        }
        if (mkdir(path, 0777) != 0 && errno != EEXIST) {
            ESP_LOGW(TAG, "建目錄 %s 失敗（errno %d）→ 上傳 %s/*.json 可能失敗",
                     path, errno, sub[i]);
        }
    }
}

// 由 mode 建 plan（切換 mode 同開機共用）。只在 paint loop／on_start 呼叫。
// playlist 的下一個：registry list 次序（同 auto_play 一致），跳過唔存在／同一個 mode。
static const nc_px_mode_t *playlist_next_mode(void)
{
    const nc_px_show_t *reg = nc_px_show();
    if (reg == NULL || reg->nlist < 2 || s_content.mode_name[0] == '\0') {
        return NULL;
    }
    int cur = -1;
    for (uint32_t i = 0; i < reg->nlist; i++) {
        if (strcmp(reg->list[i], s_content.mode_name) == 0) {
            cur = (int)i;
            break;
        }
    }
    const int start = (cur >= 0) ? (cur + 1) : 0;
    for (uint32_t n = 0; n < reg->nlist; n++) {
        const int idx = (start + n) % reg->nlist;
        const nc_px_mode_t *m = nc_px_mode_find(reg->list[idx]);
        if (m != NULL && strcmp(m->name, s_content.mode_name) != 0) {
            return m;
        }
    }
    return NULL;
}

// 播放期間預建下一個 mode 的 layout＋flat（PSRAM）。成功後 next_ready = true，
// 切換嗰下只係 swap，唔使再 build／讀檔。
static void content_prepare_next(void)
{
    if (s_content.next_ready) {
        return;                        // 已預建好：唔好覆寫 why
    }
    if (!s_content.ready || s_content.px == 0) {
        s_content.next_why = 6;
        return;
    }
    const nc_px_mode_t *m = playlist_next_mode();
    if (m == NULL) {
        s_content.next_why = 2;
        return;
    }
    if (s_content.next_assets == NULL) {
        s_content.next_assets = (nc_px_asset_tbl_t *)heap_caps_malloc(
            sizeof(nc_px_asset_tbl_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_content.next_assets == NULL) {
            return;
        }
        memset(s_content.next_assets, 0, sizeof(*s_content.next_assets));
    }
    if (build_asset_tbl_into(s_content.next_assets, m) != ESP_OK) {
        s_content.next_why = 3;
        ESP_LOGW(TAG, "預建下一張：%s 效果表失敗", m->name);
        return;
    }
    const nc_px_map_t *map = select_map(m);
    if (map == NULL) {
        s_content.next_why = 4;
        ESP_LOGW(TAG, "預建下一張：%s 冇 map", m->name);
        return;
    }
    const int64_t t0 = esp_timer_get_time();
    nc_px_layout_free(&s_content.next_layout);
    const uint32_t ckey = nc_px_layout_cache_key(m->hash, map->hash, s_content.px,
                                                   assets_hash(s_content.assets));
    char cpath[96];
    (void)snprintf(cpath, sizeof(cpath), "%s/cache/%08lx.layout", s_content.pixel_dir,
                   (unsigned long)ckey);
    esp_err_t e = nc_px_layout_load(&s_content.next_layout, cpath, ckey,
                                    s_content.next_assets);
    if (e != ESP_OK || s_content.next_layout.njobs == 0) {
        if (e == ESP_OK) {
            nc_px_layout_free(&s_content.next_layout);
        }
        const nc_px_hw_inv_t inv = { .hw = NC_PX_HW_WS2812, .base = 0, .px = s_content.px };
        e = nc_px_layout_build(m, map, s_content.next_assets, &inv, 1,
                               &s_content.next_layout);
    }
    if (e != ESP_OK || s_content.next_layout.njobs == 0) {
        nc_px_layout_free(&s_content.next_layout);
        s_content.next_why = 5;
        ESP_LOGW(TAG, "預建下一張：%s 建表失敗（%s、jobs=%u）", m->name,
                 esp_err_to_name(e), (unsigned)s_content.next_layout.njobs);
        return;
    }
    (void)nc_px_layout_flatify(&s_content.next_layout);   // 默認 PSRAM
    snprintf(s_content.next_mode_name, sizeof(s_content.next_mode_name), "%s", m->name);
    snprintf(s_content.next_map_name, sizeof(s_content.next_map_name), "%s", map->name);
    s_content.next_ready = true;
    s_content.next_why = 1;
    s_content.next_us = (uint32_t)(esp_timer_get_time() - t0);
    ESP_LOGI(TAG, "預建下一張：mode=%s、runs=%u、flat=%u B、%u us",
             m->name, (unsigned)s_content.next_layout.nruns,
             (unsigned)nc_px_layout_flat_bytes(&s_content.next_layout),
             (unsigned)s_content.next_us);
}

// 切換到已預建的下一張：swap layout／assets，唔重建。
static esp_err_t content_swap_next(const nc_px_mode_t *mode)
{
    const int64_t t0 = esp_timer_get_time();
    nc_px_layout_free(&s_content.layout);
    s_content.layout = s_content.next_layout;
    memset(&s_content.next_layout, 0, sizeof(s_content.next_layout));
    nc_px_asset_tbl_t *tmp = s_content.assets;
    s_content.assets = s_content.next_assets;
    s_content.next_assets = tmp;
    snprintf(s_content.map_name, sizeof(s_content.map_name), "%s", s_content.next_map_name);
    snprintf(s_content.mode_name, sizeof(s_content.mode_name), "%s", mode->name);
    s_content.white = (mode->white > 100) ? 100 : mode->white;
    s_content.step = 0;
    s_content.mode_frames = 0;
    s_content.mode_limit = (mode->maxF > 0) ? mode->maxF : 500;
    s_content.ready = true;
    s_content.fallback = false;
    s_content.next_ready = false;
    s_content.next_mode_name[0] = '\0';
    s_last_eff_seq = 0;
    s_sf_seq = 0;
    s_sf_done = 0;
    s_plan_gen++;
    nc_pipe_aprog_publish(s_content.layout.aprog, s_content.layout.naprog, s_plan_gen);
    s_last_apply_us = (uint32_t)(esp_timer_get_time() - t0);
    ESP_LOGI(TAG, "換場（預建 swap）：%s、%u us、runs=%u", mode->name,
             (unsigned)s_last_apply_us, (unsigned)s_content.layout.nruns);
    return ESP_OK;
}

static esp_err_t content_apply_mode_locked(const nc_px_mode_t *mode)
{
    if (mode == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_content.next_ready && strcmp(mode->name, s_content.next_mode_name) == 0) {
        return content_swap_next(mode);
    }
    const int64_t t0 = esp_timer_get_time();

    // map：mode.mapping → entry group 前綴 → map 表第一個。
    const nc_px_map_t *map = select_map(mode);
    if (map == NULL) {
        ESP_LOGW(TAG, "mode %s 冇對應 map", mode->name);
        return ESP_ERR_NOT_FOUND;
    }
    snprintf(s_content.map_name, sizeof(s_content.map_name), "%s", map->name);

    // white：mode JSON 的 "white"／"white_mix"（0..100，預設 0）。
    s_content.white = (mode->white > 100) ? 100 : mode->white;

    // plan：mode + map + assets + inventory（8×8 Matrix = 一條 WS2812、base 0）。
    const esp_err_t te = build_asset_tbl(mode);
    if (te != ESP_OK) {
        ESP_LOGW(TAG, "mode %s：效果表配置失敗（entry=%u）", mode->name,
                 (unsigned)mode->nentry);
        return te;
    }
    const nc_px_hw_inv_t inv = {
        .hw = NC_PX_HW_WS2812,
        .base = 0,
        .px = s_content.px,
    };
    nc_px_layout_free(&s_content.layout);

    // 組合的快取：同一份 mode／map＋同一個 px → 直接讀已編譯結果（固定時間，唔使再計）。
    // 內容一改（hash 變）或 px 改，key 就唔夾 → 當失效，重建並覆寫。
    // 每個組合一個檔案（key 由 mode／map 內容＋px 決定）：切返之前用過的組合可以即刻讀。
    const uint32_t ckey = nc_px_layout_cache_key(mode->hash, map->hash, s_content.px,
                                                      assets_hash(s_content.assets));
    char cpath[96];
    (void)snprintf(cpath, sizeof(cpath), "%s/cache/%08lx.layout", s_content.pixel_dir,
                   (unsigned long)ckey);
    const int64_t t_load0 = esp_timer_get_time();
    esp_err_t pe = nc_px_layout_load(&s_content.layout, cpath, ckey, s_content.assets);
    const uint32_t load_us = (uint32_t)(esp_timer_get_time() - t_load0);
    if (pe == ESP_OK && s_content.layout.njobs > 0) {
        ESP_LOGI(TAG, "組合由快取讀取：%s（%u 條目、%u 段、%u us）", cpath,
                 (unsigned)s_content.layout.njobs, (unsigned)s_content.layout.nruns,
                 (unsigned)load_us);
    } else {
        if (pe == ESP_OK) {
            nc_px_layout_free(&s_content.layout);
        }
        const int64_t tb0 = esp_timer_get_time();
        pe = nc_px_layout_build(mode, map, s_content.assets, &inv, 1, &s_content.layout);
        const uint32_t build_us = (uint32_t)(esp_timer_get_time() - tb0);
        if (pe == ESP_OK && s_content.layout.njobs > 0) {
            // 快取只對「表細」的組合有意義：表大（例如完全隨機 ~230 KB）時，
            // FAT 讀取同重建差不多，存表反而多一次寫入。門檻 32 KB。
            const size_t cache_bytes = nc_px_layout_cache_bytes(&s_content.layout);
            if (cache_bytes <= 32768u) {
                const int64_t ts0 = esp_timer_get_time();
                const esp_err_t se = layout_cache_save(&s_content.layout, cpath, ckey,
                                                       cache_bytes);
                const uint32_t save_us = (uint32_t)(esp_timer_get_time() - ts0);
                ESP_LOGI(TAG, "組合重建：build %u us、快取 %u B、寫入 %u us（%s）",
                         build_us, (unsigned)cache_bytes, save_us,
                         (se == ESP_OK) ? "ok" : esp_err_to_name(se));
            } else {
                ESP_LOGW(TAG, "組合重建：build %u us、快取表 %u B 太大（>32 KB）→ 唔存"
                              "（讀表同重建差不多）", build_us, (unsigned)cache_bytes);
            }
        }
    }
    if (pe != ESP_OK || s_content.layout.njobs == 0) {
        ESP_LOGW(TAG, "layout 失敗：mode=%s map=%s jobs=%u runs=%u skipped=%u"
                      "（effect 唔存在 %u、group 唔存在 %u、write 唔識 %u、空 %u、"
                      "效果超上限 %u、program 壞 %u、記憶體 %u）（%s）",
                 mode->name, map->name, (unsigned)s_content.layout.njobs,
                 (unsigned)s_content.layout.nruns, (unsigned)s_content.layout.skipped,
                 (unsigned)s_content.layout.skip_no_asset,
                 (unsigned)s_content.layout.skip_no_group,
                 (unsigned)s_content.layout.skip_bad_write,
                 (unsigned)s_content.layout.skip_empty,
                 (unsigned)s_content.layout.skip_asset_full,
                 (unsigned)s_content.layout.skip_prog,
                 (unsigned)s_content.layout.skip_oom,
                 esp_err_to_name(pe));
        return ESP_ERR_NOT_FOUND;
    }

    snprintf(s_content.mode_name, sizeof(s_content.mode_name), "%s", mode->name);
    s_content.step = 0;
    s_content.mode_frames = 0;
    s_content.mode_limit = (mode->maxF > 0) ? mode->maxF : 500;
    if (s_content.layout.flat_err != 0) {
        ESP_LOGW(TAG, "6 B 表建唔到（%s）：行 runs（熱路徑會慢）",
                 esp_err_to_name((esp_err_t)s_content.layout.flat_err));
    }
    s_content.ready = true;
    s_content.fallback = false;
    s_last_eff_seq = 0;
    s_sf_seq = 0;
    s_sf_done = 0;
    s_plan_gen++;
    nc_pipe_aprog_publish(s_content.layout.aprog, s_content.layout.naprog, s_plan_gen);
    s_last_apply_us = (uint32_t)(esp_timer_get_time() - t0);

    ESP_LOGI(TAG, "選中 mode：%s（id=%u）mapping=%s white=%u%%；map=%s（%u groups）",
             mode->name, (unsigned)mode->id,
             (mode->mapping[0] != '\0') ? mode->mapping : "-",
             (unsigned)s_content.white, map->name, (unsigned)map->ngroups);
    // 記憶體預算（由實際 plan 計，唔用固定上限）：呢四個數加起來就是 pair 的 steady-state。
    ESP_LOGI(TAG, "pair 記憶體：canvas %u B、值流 %u B（×2）、runs %u × 16 B、"
                  "jobs %u × 8 B、aprog %u × %u B（全部 PSRAM）",
             (unsigned)(s_content.layout.px * NC_PX_BPP),
             (unsigned)(s_content.layout.nvals * 2u),
             (unsigned)s_content.layout.nruns, (unsigned)s_content.layout.njobs,
             (unsigned)s_content.layout.naprog, (unsigned)sizeof(nc_px_aprog_t));
    if (s_content.layout.skipped > 0) {
        ESP_LOGW(TAG, "layout 有 %u 條目被跳過：effect 唔存在 %u、group 唔存在 %u、"
                      "write 唔識 %u、空 %u、效果超上限 %u、program 壞 %u、記憶體 %u",
                 (unsigned)s_content.layout.skipped,
                 (unsigned)s_content.layout.skip_no_asset,
                 (unsigned)s_content.layout.skip_no_group,
                 (unsigned)s_content.layout.skip_bad_write,
                 (unsigned)s_content.layout.skip_empty,
                 (unsigned)s_content.layout.skip_asset_full,
                 (unsigned)s_content.layout.skip_prog,
                 (unsigned)s_content.layout.skip_oom);
    }
    ESP_LOGI(TAG, "layout：px=%u jobs=%u runs=%u aprog=%u skipped=%u；rebuild %u us；環 %s",
             (unsigned)s_content.layout.px, (unsigned)s_content.layout.njobs,
             (unsigned)s_content.layout.nruns, (unsigned)s_content.layout.naprog,
             (unsigned)s_content.layout.skipped, (unsigned)s_last_apply_us,
             nc_px_ring_name(s_content.q));
    return ESP_OK;
}

// 0xF06F：重新掃四層，按目前 mode 名重建 plan（名唔存在 → registry list[0]）。
static esp_err_t content_reload(void)
{
    if (s_content.pixel_dir[0] == '\0' || s_plan_mtx == NULL) {
        content_set_fallback("reload：未有 pixel_dir／plan mutex");
        return ESP_ERR_INVALID_STATE;
    }
    // parse + plan rebuild 全程持 plan mutex；effect 只會在 swap 前／後拿到完整 plan。
    if (xSemaphoreTake(s_plan_mtx, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t le = nc_px_layer_load_all(s_content.pixel_dir);
    log_tree(s_content.pixel_dir);
    if (le != ESP_OK) {
        content_set_fallback("reload：四層載入失敗");
        xSemaphoreGive(s_plan_mtx);
        return le;
    }
    const nc_px_show_t *reg = nc_px_show();
    s_content.auto_play = (reg != NULL) ? reg->auto_play : false;

    const nc_px_mode_t *mode = NULL;
    if (s_content.mode_name[0] != '\0') {
        mode = nc_px_mode_find(s_content.mode_name);
    }
    if (mode == NULL && reg != NULL && reg->nlist > 0) {
        mode = nc_px_mode_find(reg->list[0]);
    }
    if (mode == NULL) {
        const nc_px_mode_tbl_t *tbl = nc_px_mode_table();
        if (tbl != NULL && tbl->n > 0) {
            mode = &tbl->item[0];
        }
    }
    if (mode == NULL) {
        content_set_fallback("reload：冇 mode");
        xSemaphoreGive(s_plan_mtx);
        return ESP_ERR_NOT_FOUND;
    }
    const esp_err_t ae = content_apply_mode_locked(mode);
    xSemaphoreGive(s_plan_mtx);
    if (ae != ESP_OK) {
        content_set_fallback("reload：plan 失敗");
        return ae;
    }
    ESP_LOGI(TAG, "reload 完成：mode=%s auto_play=%d", mode->name,
             s_content.auto_play ? 1 : 0);
    return ESP_OK;
}

// registry auto_play：播夠 mode_limit 幀就按 list 次序換下一個。
// 手動 0xF06D 不影響 auto_play；下一次照 list 中目前 mode 的下一項。
static void content_auto_advance(void)
{
    if (!s_content.auto_play) {
        return;
    }
    s_content.mode_frames++;
    if (s_content.mode_frames < s_content.mode_limit) {
        return;
    }
    s_content.mode_frames = 0;
    const nc_px_show_t *reg = nc_px_show();
    if (reg == NULL || reg->nlist < 2) {
        return;
    }
    int cur = -1;
    for (uint32_t i = 0; i < reg->nlist; i++) {
        if (strcmp(reg->list[i], s_content.mode_name) == 0) {
            cur = (int)i;
            break;
        }
    }
    const int start = (cur >= 0) ? (cur + 1) : 0;
    for (uint32_t n = 0; n < reg->nlist; n++) {
        const int idx = (start + n) % reg->nlist;
        const nc_px_mode_t *mode = nc_px_mode_find(reg->list[idx]);
        if (mode != NULL) {
            char old[NC_PX_LAYER_NAME_MAX];
            snprintf(old, sizeof(old), "%s", s_content.mode_name);
            if (content_apply_mode(mode) == ESP_OK) {
                ESP_LOGI(TAG, "auto_play：%s → %s（每 %u 幀）", old,
                         mode->name, (unsigned)s_content.mode_limit);
                return;
            }
        }
    }
}

static esp_err_t content_apply_mode(const nc_px_mode_t *mode)
{
    if (s_plan_mtx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_plan_mtx, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t e = content_apply_mode_locked(mode);
    xSemaphoreGive(s_plan_mtx);
    return e;
}

static void content_start_pipeline(void)
{
    // ① 環：render 先起（同 level、order 10），所以這裡應該搵到。
    s_content.q = find_prd_ring();
    if (s_content.q == NULL) {
        ESP_LOGW(TAG, "找不到 prd 環 → content 唔會出畫面（render 任務有起嗎？）");
        return;
    }
    s_content.px = nc_px_ring_px(s_content.q);
    if (s_content.px == 0) {
        content_set_fallback("prd 環 px=0");
        return;
    }

    // ①b 兩塊「大但唔熱」的表放 PSRAM，避免佔 internal BSS：
    //     asset 表（表頭 ＋ 按 entry 數的效果，262 B／個）只在換場讀；
    //     0xF06B 快照 2 KB 只在主機 poll 時讀。
    if (s_content.assets == NULL) {
        s_content.assets = (nc_px_asset_tbl_t *)heap_caps_malloc(
            sizeof(nc_px_asset_tbl_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_content.assets == NULL) {
            content_set_fallback("asset 表配置失敗（PSRAM）");
            return;
        }
        memset(s_content.assets, 0, sizeof(*s_content.assets));
    }
    if (s_content.last == NULL) {
        s_content.last = (uint8_t *)heap_caps_malloc(SHOWFRAME_PX_MAX * NC_PX_BPP,
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_content.last == NULL) {
            content_set_fallback("0xF06B 快照配置失敗（PSRAM）");
            return;
        }
        memset(s_content.last, 0, SHOWFRAME_PX_MAX * NC_PX_BPP);
    }

    // ② 檔案樹：/data 由 store 掛載（B02 在任務啟動之前）。
    const char *base = nc_store_fs_base();
    if (base == NULL) {
        content_set_fallback("/data 未掛載（nc_store_fs_base()=NULL）");
        return;
    }
    char pixel_dir[64];
    if (snprintf(pixel_dir, sizeof(pixel_dir), "%s/pixel", base) >= (int)sizeof(pixel_dir)) {
        content_set_fallback("pixel 路徑太長");
        return;
    }
    snprintf(s_content.pixel_dir, sizeof(s_content.pixel_dir), "%s", pixel_dir);
    ensure_pixel_dirs(pixel_dir);          // 先建 map/、modes/（上傳/FILE_BEGIN 需要）
    const esp_err_t le = nc_px_layer_load_all(pixel_dir);
    log_tree(pixel_dir);
    if (le != ESP_OK) {
        content_set_fallback("四層載入失敗（/data/pixel 缺失或 effects 全壞）");
        return;
    }

    // ③ mode：registry 的 list[0]；冇 registry／搵唔到 → mode 表第一個。
    const nc_px_mode_tbl_t *modes = nc_px_mode_table();
    if (modes == NULL || modes->n == 0) {
        content_set_fallback("冇任何 mode（/data/pixel/modes）");
        return;
    }
    const nc_px_mode_t *mode = NULL;
    const nc_px_show_t *show = nc_px_show();
    if (show != NULL && show->nlist > 0) {
        mode = nc_px_mode_find(show->list[0]);
        if (mode == NULL) {
            ESP_LOGW(TAG, "registry list[0]=\"%s\" 搵唔到 → 用 mode 表第一個",
                     show->list[0]);
        }
    }
    if (mode == NULL) {
        mode = &modes->item[0];
    }
    const nc_px_show_t *reg = nc_px_show();
    s_content.auto_play = (reg != NULL) ? reg->auto_play : false;
    if (content_apply_mode(mode) != ESP_OK) {
        content_set_fallback("plan 冇有效 job（effect／group 名對唔上？）");
        return;
    }
    ESP_LOGI(TAG, "pair 就緒：%u px、prd 環 %s", (unsigned)s_content.px,
             nc_px_ring_name(s_content.q));
#if CONFIG_NETCORE_PIXEL_APPLY_PIPE
    (void)pipe_start();
#endif
    cache_prebuild_missing();          // 開機檢查一次，缺的組合表補建
}

// 開機只預建 registry list 頭 N 個 mode 的組合表（N=Kconfig，預設 4；0=唔預建）。
// 其餘在 0xF06D 切 mode 時才 build／load。避免 mode 多時開機連續 build 39–137 ms × N，
// 亦避免一次過寫滿 FAT；cache 總量另有 layout_cache_save() 的預算／LRU 把關。
static void cache_prebuild_missing(void)
{
    const uint32_t max_modes = (uint32_t)CONFIG_NETCORE_PIXEL_LAYOUT_PREBUILD_MAX;
    const nc_px_mode_tbl_t *modes = nc_px_mode_table();
    if (max_modes == 0 || modes == NULL || modes->n == 0 || s_content.px == 0) {
        return;
    }
    const nc_px_show_t *reg = nc_px_show();
    const nc_px_mode_t **cand = (const nc_px_mode_t **)malloc(
        (size_t)(modes->n ? modes->n : 1u) * sizeof(*cand));
    if (cand == NULL) {
        return;
    }
    uint32_t n = 0;
    // 先跟 registry list（播放清單次序），再加其餘 mode（去重）。
    if (reg != NULL) {
        for (uint32_t i = 0; i < reg->nlist && n < modes->n; i++) {
            const nc_px_mode_t *m = nc_px_mode_find(reg->list[i]);
            bool dup = false;
            for (uint32_t k = 0; k < n; k++) {
                if (cand[k] == m) {
                    dup = true;
                    break;
                }
            }
            if (m != NULL && !dup) {
                cand[n++] = m;
            }
        }
    }
    for (uint32_t i = 0; i < modes->n; i++) {
        const nc_px_mode_t *m = &modes->item[i];
        bool dup = false;
        for (uint32_t k = 0; k < n; k++) {
            if (cand[k] == m) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            cand[n++] = m;
        }
    }
    if (n > max_modes) {
        n = max_modes;
    }

    nc_px_asset_tbl_t *tmp = nc_px_asset_tbl_alloc(0);
    if (tmp == NULL) {
        ESP_LOGW(TAG, "開機預建：臨時 asset 表配置失敗 → 略過");
        free(cand);
        return;
    }
    uint32_t present = 0, built = 0, too_big = 0, failed = 0;
    const int64_t t0 = esp_timer_get_time();
    const nc_px_hw_inv_t inv = { .hw = NC_PX_HW_WS2812, .base = 0, .px = s_content.px };
    for (uint32_t ci = 0; ci < n; ci++) {
        const nc_px_mode_t *m = cand[ci];
        const nc_px_map_t *map = select_map(m);
        if (map == NULL) {
            failed++;
            continue;
        }
        if (build_asset_tbl_into(tmp, m) != ESP_OK) {
            failed++;
            continue;
        }
        const uint32_t key = nc_px_layout_cache_key(m->hash, map->hash, s_content.px,
                                                   assets_hash(s_content.assets));
        char path[96];
        (void)snprintf(path, sizeof(path), "%s/cache/%08lx.layout", s_content.pixel_dir,
                       (unsigned long)key);
        nc_px_layout_t probe;
        if (nc_px_layout_load(&probe, path, key, tmp) == ESP_OK) {
            nc_px_layout_free(&probe);
            present++;
            continue;
        }
        nc_px_layout_t lay;
        if (nc_px_layout_build(m, map, tmp, &inv, 1, &lay) != ESP_OK || lay.njobs == 0) {
            nc_px_layout_free(&lay);
            failed++;
            continue;
        }
        const size_t cache_bytes = nc_px_layout_cache_bytes(&lay);
        if (cache_bytes > 32768u) {
            too_big++;
        } else if (layout_cache_save(&lay, path, key, cache_bytes) == ESP_OK) {
            built++;
        } else {
            failed++;                        // 預算不足／寫入失敗
        }
        nc_px_layout_free(&lay);
    }
    nc_px_asset_tbl_free(tmp);
    free(cand);
    s_prebuild.present = present;
    s_prebuild.built = built;
    s_prebuild.too_big = too_big;
    s_prebuild.failed = failed;
    s_prebuild.ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    uint32_t cfiles = 0, cbytes = 0;
    layout_cache_scan(&cfiles, &cbytes, NULL, 0, NULL);
    ESP_LOGI(TAG, "開機預建組合表：候選 %u／上限 %u、已有 %u、新建 %u、太大 %u、失敗 %u"
                  "（%u ms、cache 預算 %u KB、現在 %u 檔／%u B、累計刪 %u）",
             (unsigned)n, (unsigned)max_modes, (unsigned)present, (unsigned)built,
             (unsigned)too_big, (unsigned)failed, (unsigned)s_prebuild.ms,
             (unsigned)(layout_cache_budget() / 1024u), (unsigned)cfiles,
             (unsigned)cbytes, (unsigned)s_cache_trimmed);
}

static esp_err_t content_start(const nc_task_res_view_t *res, int nres)
{
    (void)res;
    (void)nres;
    if (s_plan_mtx == NULL) {
        s_plan_mtx = xSemaphoreCreateMutex();
    }
    if (s_plan_mtx == NULL) {
        return ESP_ERR_NO_MEM;
    }
    content_state_reset();
    content_start_pipeline();
    // 永遠回 ESP_OK：載入失敗已經 fallback（填黑），唔好因為內容問題令 boot 失敗。
    return ESP_OK;
}

static void content_stop(void)
{
#if CONFIG_NETCORE_PIXEL_APPLY_PIPE
    pipe_stop();
#endif
    if (s_content.canvas != NULL) {
        heap_caps_free(s_content.canvas);
        s_content.canvas = NULL;
    }
    if (s_content.assets != NULL) {
        nc_px_asset_tbl_free(s_content.assets);   // 連 item 一齊放（owned）
        s_content.assets = NULL;
    }
    nc_px_layout_free(&s_content.next_layout);
    s_content.next_ready = false;
    if (s_content.next_assets != NULL) {
        nc_px_asset_tbl_free(s_content.next_assets);
        s_content.next_assets = NULL;
    }
    if (s_content.last != NULL) {
        heap_caps_free(s_content.last);
        s_content.last = NULL;
    }
    s_content.ready = false;
    s_content.q = NULL;
}

// ═══════════════════════ 0xF06B SHOWFRAME ═══════════════════════
//
// 回覆 JSON：{"px":N,"seq":M,"hex":"<4 B/px 的 hex>"}（64 px = 512 hex 字元）。
// 冇快照（show 未起）→ {"err":"nodata"}；px > 512 或 hex 塞唔落回覆緩衝 →
// {"err":"big","px":N}。
static void on_showframe(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    (void)f;                       // 請求冇參數；永遠回最近一幀
    if (ctx == NULL || ctx->send == NULL || ctx->decoder == NULL) {
        return;
    }
    s_snap_wanted = true;          // 由下一幀的環槽複製（零拷貝路徑唔再寫 canvas）
    uint8_t *fb = nc4_decoder_reply_buf(ctx->decoder);
    if (fb == NULL) {
        return;
    }
    uint8_t *data = fb + NC4_HDR_LEN;
    char *js = (char *)(data + 2);               // 2 B = JSON 長度（NC4 回覆格式）
    const size_t cap = NC4_REPLY_JSON_MAX + 1;   // + NUL；回覆幀另有 4 B CRC 餘量
    int n = 0;

    // 快照由 paint loop 更新（另一個任務、可能另一核）→ 短臨界區。
    // hex 直接寫入 decoder 的回覆緩衝，唔喺 decode task 的棧上開大 buffer。
    taskENTER_CRITICAL(&s_content.lock);
    if (!s_content.last_valid || s_content.last == NULL) {
        n = snprintf(js, cap, "{\"err\":\"nodata\"}");
    } else if (s_content.last_px > SHOWFRAME_PX_MAX) {
        n = snprintf(js, cap, "{\"err\":\"big\",\"px\":%u}", (unsigned)s_content.last_px);
    } else {
        const uint32_t px = s_content.last_px;
        // 預算：前綴 ≤ 40 B ＋ px×8 hex ＋ 2 B 收尾；保守留 80 B 餘量。
        if (px > (uint32_t)((NC4_REPLY_JSON_MAX - 80u) / 8u)) {
            n = snprintf(js, cap, "{\"err\":\"big\",\"px\":%u}", (unsigned)px);
        } else {
            static const char hexd[] = "0123456789abcdef";
            n = snprintf(js, cap, "{\"px\":%u,\"seq\":%u,\"mode\":\"%s\","
                         "\"white\":%u,\"apply_us\":%u,\"fallback\":\"%s\",\"hex\":\"",
                         (unsigned)px, (unsigned)s_content.last_seq, s_content.mode_name,
                         (unsigned)s_content.white, (unsigned)s_last_apply_us,
                         s_content.fallback_why);
            if (n > 0) {
                for (uint32_t i = 0; i < px * NC_PX_BPP; i++) {
                    const uint8_t b = s_content.last[i];
                    js[n++] = hexd[b >> 4];
                    js[n++] = hexd[b & 0x0F];
                }
                js[n++] = '"';
                js[n++] = '}';
                js[n] = '\0';
            }
        }
    }
    taskEXIT_CRITICAL(&s_content.lock);

    if (n <= 0) {
        return;
    }
    if ((size_t)n > NC4_REPLY_JSON_MAX) {
        n = NC4_REPLY_JSON_MAX;                  // 保險：正常路徑一定 ≤ 上限
    }
    data[0] = (uint8_t)n;
    data[1] = (uint8_t)((unsigned)n >> 8);
    const int total = nc4_pack_into(fb, NC4_REPLY_FRAME_MAX, NC4_CMD_SHOWFRAME_RSP,
                                    nc4_reply_addr(ctx), data, (size_t)(2 + n));
    if (total > 0) {
        ctx->send(ctx->send_arg, fb, (size_t)total);
    }
}

// ═══════════════════════ 任務宣告／指令表 ═══════════════════════

static const nc_task_loop_t s_loops[] = {
    {
        // 生產者：檔案樹 → 環。優先級低 render 一級（同 pixel_task 的 prd 一樣）。
        .name = "paint",
        .fn = content_loop,
        .stack = 4096,
        .prio = NC_PRIO_OTHERS_MAX - 1,
        .core = 1,
        .layer = 5,
        .period_ms = 0,               // 環滿自己延遲，有空間就填
    },
};

static const nc_task_t s_task = {
    .name = "pixel_paint_task",        // 2026-11-25 正名（tasks.json 舊名 pair 由 alias 讀）
    .kind = NC_TASK_NORMAL,
    .level = 4,
    .order = 21,
    .on_start = content_start,
    .on_stop = content_stop,
    .loops = s_loops,
    .nloops = 1,
};

NC_TASK_AUTO_REG(pixel_paint_task, &s_task);

#define NC4_CMD_SHOWMODE     0xF06D
#define NC4_CMD_SHOWMODE_RSP 0xF06E

// 通用 JSON 回覆（同 effect_bench 一樣：decoder 預配的回覆緩衝，不放棧）。
static void reply_json(nc4_ctx_t *ctx, uint16_t cmd, const char *json)
{
    if (ctx == NULL || ctx->send == NULL || ctx->decoder == NULL) {
        return;
    }
    uint8_t *fb = nc4_decoder_reply_buf(ctx->decoder);
    if (fb == NULL) {
        return;
    }
    size_t n = strlen(json);
    uint8_t *data = fb + NC4_HDR_LEN;
    if (n > NC4_REPLY_JSON_MAX) {
        n = (size_t)snprintf((char *)data + 2, NC4_REPLY_JSON_MAX + 1,
                             "{\"err\":\"reply_too_big\"}");
    } else {
        memcpy(data + 2, json, n);
    }
    data[0] = (uint8_t)n;
    data[1] = (uint8_t)(n >> 8);
    const int total = nc4_pack_into(fb, NC4_REPLY_FRAME_MAX, cmd,
                                    nc4_reply_addr(ctx), data, 2 + n);
    if (total > 0) {
        ctx->send(ctx->send_arg, fb, (size_t)total);
    }
}

// payload [mode_id:u8]；只排隊，paint loop 喺下一幀邊界 rebuild plan。
// 回覆即刻回目前 mode（未切換）；主機用 0xF06B 確認新 frame。
static void on_showmode(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    if (f->len < 1) {
        reply_json(ctx, NC4_CMD_SHOWMODE_RSP, "{\"err\":\"mode_id\"}");
        return;
    }
    const uint8_t id = f->data[0];
    const nc_px_mode_tbl_t *tbl = nc_px_mode_table();
    bool found = false;
    if (tbl != NULL) {
        for (uint8_t i = 0; i < tbl->n; i++) {
            if (tbl->item[i].id == id) {
                found = true;
                break;
            }
        }
    }
    if (!found) {
        reply_json(ctx, NC4_CMD_SHOWMODE_RSP, "{\"err\":\"mode_not_found\"}");
        return;
    }
    s_pending_mode = id;
    char j[160];
    (void)snprintf(j, sizeof(j),
                   "{\"ok\":1,\"queued\":%u,\"mode\":\"%s\",\"white\":%u}",
                   (unsigned)id, s_content.mode_name, (unsigned)s_content.white);
    reply_json(ctx, NC4_CMD_SHOWMODE_RSP, j);
}

#define NC4_CMD_RELOAD     0xF06F
#define NC4_CMD_RELOAD_RSP 0xF070

static void on_reload(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    (void)f;
    s_pending_reload = true;
    reply_json(ctx, NC4_CMD_RELOAD_RSP, "{\"ok\":1,\"queued\":\"reload\"}");
}

#define NC4_CMD_PIPESTATS 0xF071
#define NC4_CMD_PIPESTATS_RSP 0xF072
#define NC4_CMD_PIPEBURN 0xF073
#define NC4_CMD_PIPEBURN_RSP 0xF074
#define NC4_CMD_PIPERESET 0xF075
#define NC4_CMD_PIPERESET_RSP 0xF076
#define NC4_CMD_PIXDIAG   0xF079
#define NC4_CMD_PIXDIAG_RSP 0xF07A
#define NC4_CMD_FLATREAD  0xF07B
#define NC4_CMD_FLATREAD_RSP 0xF07C
#define NC4_CMD_FLATTIME  0xF07D
#define NC4_CMD_FLATTIME_RSP 0xF07E
#define NC4_CMD_APPLYTIME 0xF07F
#define NC4_CMD_APPLYTIME_RSP 0xF080
#define NC4_CMD_FPS       0xF081
#define NC4_CMD_FPS_RSP   0xF082

static void on_pipestats(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    (void)f;
    nc_pipe_stats_t st = {0};
    nc_px_stat_t st_render = {0};
    nc_pipe_stats_get(&st);
    nc_render_get_stats(&st_render);
    nc_px_q_stat_t qs = {0};
    const char *ring_name = (s_content.q != NULL) ? nc_px_ring_name(s_content.q) : "-";
    const uint32_t fill = (s_content.q != NULL) ? nc_px_ring_fill(s_content.q) : 0;
    if (s_content.q != NULL) {
        nc_px_ring_stats(s_content.q, &qs);
    }
    char j[1024];
    (void)snprintf(j, sizeof(j),
        "{\"eff\":{\"n\":%u,\"avg_us\":%u,\"max_us\":%u,\"last_us\":%u},"
        "\"pair\":{\"n\":%u,\"avg_us\":%u,\"max_us\":%u,\"last_us\":%u},"
        "\"clear\":{\"avg_us\":%u,\"max_us\":%u},"
        "\"apply\":{\"avg_us\":%u,\"max_us\":%u},"
        "\"white\":{\"avg_us\":%u,\"max_us\":%u},"
        "\"commit\":{\"avg_us\":%u,\"max_us\":%u},"
        "\"lat\":{\"avg_us\":%u,\"max_us\":%u},"
        "\"render\":{\"frames\":%u,\"fresh\":%u,\"starved\":%u,\"late\":%u,"
        "\"frame_us\":%u,\"frame_max_us\":%u,\"enc_us\":%u,\"tx_us\":%u,\"fps_x10\":%u},"
        "\"ring\":\"%s\",\"fill\":%u,\"puts\":%u,\"drops\":%u,"
        "\"full\":%u}",
        (unsigned)st.eff_n, (unsigned)st.eff_avg_us,
        (unsigned)st.eff_max_us, (unsigned)st.eff_last_us,
        (unsigned)st.pair_n, (unsigned)st.pair_avg_us,
        (unsigned)st.pair_max_us, (unsigned)st.pair_last_us,
        (unsigned)st.clear_avg_us, (unsigned)st.clear_max_us,
        (unsigned)st.apply_avg_us, (unsigned)st.apply_max_us,
        (unsigned)st.white_avg_us, (unsigned)st.white_max_us,
        (unsigned)st.commit_avg_us, (unsigned)st.commit_max_us,
        (unsigned)st.lat_avg_us, (unsigned)st.lat_max_us,
        (unsigned)st_render.frames, (unsigned)st_render.fresh,
        (unsigned)st_render.starved, (unsigned)st_render.late,
        (unsigned)st_render.frame_us, (unsigned)st_render.frame_max_us,
        (unsigned)st_render.enc_us, (unsigned)st_render.tx_us,
        (unsigned)st_render.fps_x10, ring_name, (unsigned)fill,
        (unsigned)qs.puts, (unsigned)qs.drops, (unsigned)st.full);
    reply_json(ctx, NC4_CMD_PIPESTATS_RSP, j);
}

// 0xF079：一個指令回答「呢個配置要幾多記憶體、有幾多、唔夠嘅話爭幾多」。
// 用戶可以按自己想法設 px／lane；超出能力時，這裡直接講清楚，唔會有隱藏上限。
static void on_pixdiag(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    (void)f;
    char rj[384] = {0};
    (void)nc_render_json(rj, sizeof(rj));
    const uint32_t px = s_content.px;
    const uint32_t slots = 3;                       // render 的 prc 環固定 3 格
    const uint32_t ring_b = px * NC_PX_BPP * slots; // px×4×slots
    const uint32_t canvas_b = px * NC_PX_BPP;
    const uint32_t vals_b = (s_content.ready) ? (s_content.layout.nvals * 2u) : 0u;
    const uint32_t runs_b = (s_content.ready) ? (s_content.layout.nruns * 16u) : 0u;
    // 6 B 落點表：衍生自 runs，熱路徑實際讀的是它（0 = 冇展開，行 runs）
    const uint32_t flat_n = (s_content.ready) ? s_content.layout.nflat : 0u;
    const uint32_t flat_b = (s_content.ready)
                                ? (uint32_t)nc_px_layout_flat_bytes(&s_content.layout) : 0u;
    char j[1000];
    (void)snprintf(j, sizeof(j),
        "{\"px\":%u,\"slots\":%u,\"ring_b\":%u,\"canvas_b\":%u,"
        "\"vals_b\":%u,\"runs_b\":%u,\"flat_n\":%u,\"flat_b\":%u,\"flat_err\":%d,"
        "\"jobs\":%u,\"runs\":%u,\"naprog\":%u,\"skipped\":%u,"
        "\"skip\":{\"no_asset\":%u,\"no_group\":%u,\"bad_write\":%u,"
        "\"empty\":%u,\"asset_full\":%u,\"prog\":%u,\"oom\":%u},"
        "\"free_int\":%u,\"free_ps\":%u,"
        "\"prebuild\":{\"present\":%u,\"built\":%u,\"too_big\":%u,"
        "\"failed\":%u,\"ms\":%u},"
        "\"mode\":\"%s\",\"show_nlist\":%u,\"pipe\":%d,\"pipe_ok\":%u,\"pipe_fail\":%u,"
        "\"next\":\"%s\",\"next_ready\":%d,\"next_us\":%u,\"next_why\":%u,"
        "\"render\":%s}",
        (unsigned)px, (unsigned)slots, (unsigned)ring_b, (unsigned)canvas_b,
        (unsigned)vals_b, (unsigned)runs_b, (unsigned)flat_n, (unsigned)flat_b,
        (int)s_content.layout.flat_err,
        (unsigned)s_content.layout.njobs, (unsigned)s_content.layout.nruns,
        (unsigned)s_content.layout.naprog, (unsigned)s_content.layout.skipped,
        (unsigned)s_content.layout.skip_no_asset, (unsigned)s_content.layout.skip_no_group,
        (unsigned)s_content.layout.skip_bad_write, (unsigned)s_content.layout.skip_empty,
        (unsigned)s_content.layout.skip_asset_full, (unsigned)s_content.layout.skip_prog,
        (unsigned)s_content.layout.skip_oom,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        (unsigned)s_prebuild.present, (unsigned)s_prebuild.built,
        (unsigned)s_prebuild.too_big, (unsigned)s_prebuild.failed,
        (unsigned)s_prebuild.ms,
        s_content.mode_name,
        (unsigned)((nc_px_show() != NULL) ? nc_px_show()->nlist : 0u),
#if CONFIG_NETCORE_PIXEL_APPLY_PIPE
        s_pipe.ready ? 1 : 0, (unsigned)s_pipe_ok, (unsigned)s_pipe_fail,
#else
        0, 0u, 0u,
#endif
        s_content.next_mode_name, s_content.next_ready ? 1 : 0,
        (unsigned)s_content.next_us, (unsigned)s_content.next_why,
        rj[0] ? rj : "{}");
    reply_json(ctx, NC4_CMD_PIXDIAG_RSP, j);
}

// 0xF07B：量 FAT 讀一張平坦表（預設 57,600 B）。payload = size(u32 LE)。
// 用途：評估「flat 表存 Flash、換場前預讀」值唔值得；會佔用 pair 任務（播放會停一下）。
static void on_flatread(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    uint32_t size = 57600;
    if (f != NULL && f->len >= 4) {
        memcpy(&size, f->data, 4);
    }
    if (size == 0 || size > (256u * 1024u)) {
        reply_json(ctx, NC4_CMD_FLATREAD_RSP, "{\"err\":\"size\"}");
        return;
    }
    const char *base = nc_store_fs_base();
    char path[96];
    if (f != NULL && f->len >= 4) {
        (void)snprintf(path, sizeof(path), "%s/flat_bench.bin",
                       (base != NULL) ? base : "/data");
    } else {
        // 冇 payload：讀板上已有嘅大檔（501 組 map ≈58 KB），量同一個 FAT 的讀速
        (void)snprintf(path, sizeof(path), "%s/map/matrix.json", s_content.pixel_dir);
        size = 0;                     // 由檔案大小決定
    }
    uint8_t *buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        reply_json(ctx, NC4_CMD_FLATREAD_RSP, "{\"err\":\"oom\"}");
        return;
    }
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        heap_caps_free(buf);
        reply_json(ctx, NC4_CMD_FLATREAD_RSP, "{\"err\":\"open\"}");
        return;
    }
    if (size == 0) {
        (void)fseek(fp, 0, SEEK_END);
        size = (uint32_t)ftell(fp);
        (void)fseek(fp, 0, SEEK_SET);
        if (size == 0 || size > (256u * 1024u)) {
            (void)fclose(fp);
            heap_caps_free(buf);
            reply_json(ctx, NC4_CMD_FLATREAD_RSP, "{\"err\":\"filesize\"}");
            return;
        }
    }
    const int64_t t0 = esp_timer_get_time();
    const size_t got = fread(buf, 1, size, fp);
    const int64_t t1 = esp_timer_get_time();
    (void)fclose(fp);
    heap_caps_free(buf);
    char j[160];
    (void)snprintf(j, sizeof(j), "{\"ok\":%d,\"bytes\":%u,\"us\":%u}",
                   (got == size) ? 1 : 0, (unsigned)got, (unsigned)(t1 - t0));
    reply_json(ctx, NC4_CMD_FLATREAD_RSP, j);
}

// 0xF07D：量「真實 layout」的 flatify 成本（runs → 6 B 表），唔係 bench 的隨機。
static void on_flattime(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    (void)f;
    if (!s_content.ready) {
        reply_json(ctx, NC4_CMD_FLATTIME_RSP, "{\"err\":\"not_ready\"}");
        return;
    }
    const uint32_t runs = s_content.layout.nruns;
    const int64_t t0 = esp_timer_get_time();
    const esp_err_t e = nc_px_layout_flatify_place(&s_content.layout, false);
    const uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
    char j[192];
    (void)snprintf(j, sizeof(j),
                   "{\"rc\":%d,\"runs\":%u,\"nflat\":%u,\"nflat_b\":%u,\"us\":%u,\"px\":%u}",
                   (int)e, (unsigned)runs, (unsigned)s_content.layout.nflat,
                   (unsigned)nc_px_layout_flat_bytes(&s_content.layout),
                   (unsigned)us, (unsigned)s_content.px);
    reply_json(ctx, NC4_CMD_FLATTIME_RSP, j);
}

// 0xF07F：同一份真實 layout，量 internal / PSRAM flat 表的每幀 apply。
static void on_applytime(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    (void)f;
    if (!s_content.ready || s_content.layout.nflat == 0) {
        reply_json(ctx, NC4_CMD_APPLYTIME_RSP, "{\"err\":\"not_ready\"}");
        return;
    }
    const uint32_t runs = s_content.layout.nruns;
    const uint32_t nflat = s_content.layout.nflat;
    const uint32_t naprog = s_content.layout.naprog;
    uint32_t total = 0;
    for (uint32_t i = 0; i < naprog; i++) {
        total += s_content.layout.aprog[i].nv;
    }
    uint32_t int_us = 0, ps_us = 0;
    uint8_t *dst = (uint8_t *)heap_caps_malloc((size_t)s_content.px * NC_PX_BPP,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint16_t *vbuf = (uint16_t *)heap_caps_malloc((size_t)total * sizeof(uint16_t),
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint16_t **vp = (uint16_t **)heap_caps_malloc((size_t)naprog * sizeof(uint16_t *),
                                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (dst == NULL || vbuf == NULL || vp == NULL) {
        heap_caps_free(dst);
        heap_caps_free(vbuf);
        heap_caps_free(vp);
        reply_json(ctx, NC4_CMD_APPLYTIME_RSP, "{\"err\":\"oom\"}");
        return;
    }
    uint32_t off = 0;
    for (uint32_t i = 0; i < naprog; i++) {
        vp[i] = vbuf + off;
        for (uint32_t k = 0; k < s_content.layout.aprog[i].nv; k++) {
            vbuf[off + k] = (uint16_t)((k * 37u + i * 101u) & 0x0FFFu);
        }
        off += s_content.layout.aprog[i].nv;
    }
    if (nc_px_layout_flatify_place(&s_content.layout, false) == ESP_OK) {
        for (int r = 0; r < 5; r++) {
            const int64_t t = esp_timer_get_time();
            (void)nc_px_layout_apply(&s_content.layout, vp, dst);
            const uint32_t us = (uint32_t)(esp_timer_get_time() - t);
            if (r == 0 || us < int_us) int_us = us;
        }
    }
    if (nc_px_layout_flatify_place(&s_content.layout, true) == ESP_OK) {
        for (int r = 0; r < 5; r++) {
            const int64_t t = esp_timer_get_time();
            (void)nc_px_layout_apply(&s_content.layout, vp, dst);
            const uint32_t us = (uint32_t)(esp_timer_get_time() - t);
            if (r == 0 || us < ps_us) ps_us = us;
        }
    }
    heap_caps_free(dst);
    heap_caps_free(vbuf);
    heap_caps_free(vp);
    char j[192];
    (void)snprintf(j, sizeof(j),
                   "{\"runs\":%u,\"nflat\":%u,\"apply_int_us\":%u,\"apply_ps_us\":%u,"
                   "\"px\":%u}", (unsigned)runs, (unsigned)nflat,
                   (unsigned)int_us, (unsigned)ps_us, (unsigned)s_content.px);
    reply_json(ctx, NC4_CMD_APPLYTIME_RSP, j);
}

// 0xF081：逐幀間隔統計（payload[0]=1 → 先 reset）；回 0xF082。
static void on_fps(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    if (f != NULL && f->len >= 1 && f->data[0] == 1) {
        nc_render_fps_reset();
    }
    static char j[900];
    if (nc_render_fps_json(j, sizeof(j)) == 0) {
        reply_json(ctx, NC4_CMD_FPS_RSP, "{\"err\":\"nodata\"}");
        return;
    }
    reply_json(ctx, NC4_CMD_FPS_RSP, j);
}

static void on_pipeburn(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    uint32_t sec = 10;
    if (f->len >= 1) {
        sec = f->data[0];
    }
    nc_pipe_stats_burn(sec);
    char j[96];
    (void)snprintf(j, sizeof(j), "{\"ok\":1,\"burn_s\":%u}", (unsigned)sec);
    reply_json(ctx, NC4_CMD_PIPEBURN_RSP, j);
}

static void on_pipereset(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    (void)f;
    nc_pipe_stats_reset();
    reply_json(ctx, NC4_CMD_PIPERESET_RSP, "{\"ok\":1,\"reset\":1}");
}

static const nc_action_t s_actions[] = {
    { NC4_CMD_SHOWFRAME, on_showframe, "pair" },
    { NC4_CMD_RELOAD, on_reload, "pair" },
    { NC4_CMD_SHOWMODE, on_showmode, "pair" },
    { NC4_CMD_PIPESTATS, on_pipestats, "pair" },
    { NC4_CMD_PIPEBURN, on_pipeburn, "pair" },
    { NC4_CMD_PIPERESET, on_pipereset, "pair" },
    { NC4_CMD_PIXDIAG, on_pixdiag, "pair" },
    { NC4_CMD_FLATREAD, on_flatread, "pair" },
    { NC4_CMD_FLATTIME, on_flattime, "pair" },
    { NC4_CMD_APPLYTIME, on_applytime, "pair" },
    { NC4_CMD_FPS, on_fps, "pair" },
};

NC_ACTION_TABLE_DEF(nc_pair_task_table, "pair", s_actions);

#endif  // CONFIG_NETCORE_CONTENT
