// nc_content_task.c — production 三任務的 pair 段（effect → pair → render）。
//
// 跟 pixel_task（內建六個效果）的分工：
//   · pixel_task：內建效果 → 環（CONFIG_NETCORE_PIXEL 的一部分，所有 project 共用）；
//   · pair  ：/data/pixel 四層 → 配對 → white mix → 環；effect 只算波形值。
// 兩者都是「prd 環的生產者」，同一個 project 只可以開其中一個（tasks.json 決定）。
//
// 為什麼要這一支而不是把四層塞入 pixel_task：
//   四層資料係「用戶內容」（Web UI 上傳、可改、可換 mode），生命週期同內建效果唔同；
//   而且 production 的 8×8 Show 唔要內建六個效果。獨立 component 用 CONFIG 開關，
//   release 映像可以有、其他 project 完全唔編（同 effect_bench 一樣）。
//
// 資料流（每幀）：
//   memset(canvas, 0) → nc_px_show_frame(plan, step, canvas)      // 四層解釋在 sw/pixel
//   → white mix（mode 的 "white" 0..100，四通道 out=(out*(100-w)+255*w)/100）
//   → nc_px_ring_begin_ex() 的 payload（4 B/px，R,G,B,W）→ commit → step++
// 環係 render 任務建立／登記的（名字含 "prd"）；這裡只負責寫。
//
// 失敗策略（唔好令 boot loop）：
//   /data 未掛載、/data/pixel 缺失、effects／mode／map 解析失敗、plan 冇 job、canvas
//   配置失敗 → log 清楚原因，進入 fallback：loop 仍然每幀填黑（render 會顯示黑），
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
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nc4.h"
#include "nc_action.h"
#include "nc_pixel_core.h"
#include "nc_prio.h"
#include "nc_px_asset.h"
#include "nc_px_layer.h"
#include "nc_px_layer_scan.h"
#include "nc_px_show.h"
#include "nc_pipeline.h"
#include "nc_store.h"
#include "nc_task.h"

static const char *TAG = "content";

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
    nc_pixel_q_t  *q;           // render 建立的 prd 環（NULL = render 未起）
    uint32_t       px;          // 環的 px（= inventory 的 px）
    uint8_t       *canvas;      // px × 4（internal heap）；fallback/未 ready 時可為 NULL
    uint32_t       step;        // 逐幀遞增的全域步數（動畫時間）
    uint8_t        white;       // 0..100；mode JSON 的 white／white_mix
    uint32_t       commits;     // 成功提交的幀數（= 0xF06B 的 seq）
    uint32_t       busy;        // 環滿而跳過的次數（自報 drops）

    nc_px_plan_t      plan;
    nc_px_asset_tbl_t assets;   // plan 用：只收 mode 引用到的 asset（最多 8 個）
    char              mode_name[NC_PX_LAYER_NAME_MAX];
    char              map_name[NC_PX_LAYER_NAME_MAX];
    char              pixel_dir[64];      // /data/pixel（reload 用）
    bool              auto_play;          // registry.json 的 auto_play
    uint32_t          mode_frames;        // 這個 mode 已播幾幀
    uint32_t          mode_limit;         // 幾幀後換下一個（maxF，0=500）

    // 0xF06B 快照：最近一次 commit 的 4 B/px 畫面（最多 SHOWFRAME_PX_MAX 顆）。
    // 由 pair loop 寫、nc4 decode task 讀 → 用 spinlock 護住。
    portMUX_TYPE  lock;
    uint8_t       last[SHOWFRAME_PX_MAX * NC_PX_BPP];
    uint32_t      last_px;
    uint32_t      last_seq;
    bool          last_valid;
} show_state_t;

static show_state_t s_content = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
};

// 0xF06D：decode task 只排隊，真正 rebuild plan 由 pair loop 喺幀邊界做
// （plan／asset table 係 content task 擁有，唔可以同 decode task 同時改）。
static volatile uint8_t s_pending_mode = 0xFF;
static volatile bool    s_pending_reload;
static uint32_t s_last_apply_us;

// pair 擁有的 plan；effect 只經 nc_pipe_plan_snapshot() 讀一份 copy。
static SemaphoreHandle_t s_plan_mtx;
static volatile uint32_t s_plan_gen;
static uint32_t s_last_eff_seq;
static nc_pipe_effect_frame_t s_eff_frame;

// 找回「效果用」的環：名字含 "prd"（render 的預設 lane 叫 i80.prd）。
static nc_pixel_q_t *find_prd_ring(void)
{
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

// 最新一幀的快照：pair loop 寫、0xF06B handler 讀。
// rgbw = NULL 代表「黑幀」（fallback 時用），會把快照內的有效像素清 0。
static void snapshot_publish(const uint8_t *rgbw, uint32_t px, uint32_t seq)
{
    const uint32_t n = (px > SHOWFRAME_PX_MAX) ? SHOWFRAME_PX_MAX : px;
    taskENTER_CRITICAL(&s_content.lock);
    if (n > 0) {
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
static bool commit_canvas(const uint8_t *rgbw)
{
    if (s_content.q == NULL || rgbw == NULL) {
        return false;
    }
    nc_px_hdr_t *hdr = NULL;
    uint8_t *p = nc_px_ring_begin_ex(s_content.q, &hdr);
    if (p == NULL) {
        s_content.busy++;
        return false;                     // 環滿：下一輪再試
    }
    memcpy(p, rgbw, (size_t)s_content.px * NC_PX_BPP);
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
    if (s_content.ready && s_content.canvas != NULL) {
        // pair 只讀 effect 的最新 fixed frame；effect 不會直接碰 canvas/ring。
        if (!nc_pipe_effect_copy(&s_eff_frame)) {
            return false;                 // effect 未出第一幀
        }
        if (s_eff_frame.plan_gen != s_plan_gen ||
            s_eff_frame.njobs != s_content.plan.njobs) {
            return false;                 // plan 剛換，effect 未趕上
        }
        if (s_eff_frame.seq == s_last_eff_seq) {
            return false;                 // 未有新 frame
        }
        memset(s_content.canvas, 0, (size_t)s_content.px * NC_PX_BPP);
        for (uint8_t j = 0; j < s_content.plan.njobs; j++) {
            nc_px_pair_apply(&s_content.plan.job[j], s_eff_frame.vals[j],
                             s_eff_frame.nv[j], s_content.canvas);
        }
        white_mix(s_content.canvas, s_content.px, s_content.white);
        if (!commit_canvas(s_content.canvas)) {
            return false;                 // 環滿：下 tick 再試
        }
        s_last_eff_seq = s_eff_frame.seq;
        snapshot_publish(s_content.canvas, s_content.px, s_content.commits);
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
        taskYIELD();                              // render（prio 更高）先跑
    }
}

// ═══════════════════════ 開機：四層 → plan ═══════════════════════

static void content_set_fallback(const char *why)
{
    s_content.ready = false;
    s_content.fallback = true;
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
    memset(&s_content.plan, 0, sizeof(s_content.plan));
    memset(&s_content.assets, 0, sizeof(s_content.assets));
    s_content.mode_name[0] = '\0';
    s_content.map_name[0] = '\0';
    s_content.pixel_dir[0] = '\0';
    s_plan_gen = 0;
    s_last_eff_seq = 0;
    s_content.auto_play = false;
    s_content.mode_frames = 0;
    s_content.mode_limit = 500;
    s_content.last_valid = false;
    s_content.last_px = 0;
    s_content.last_seq = 0;
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
static void build_asset_tbl(const nc_px_mode_t *mode)
{
    memset(&s_content.assets, 0, sizeof(s_content.assets));
    for (uint8_t i = 0; i < mode->nentry; i++) {
        const nc_px_asset_t *a = nc_px_asset_find(mode->entry[i].effect);
        if (a == NULL) {
            continue;                     // 未知 effect：plan 會記入 skipped
        }
        bool dup = false;
        for (uint8_t k = 0; k < s_content.assets.n; k++) {
            if (strcmp(s_content.assets.item[k].name, a->name) == 0) {
                dup = true;
                break;
            }
        }
        if (dup || s_content.assets.n >= NC_PX_ASSET_MAX) {
            continue;
        }
        s_content.assets.item[s_content.assets.n++] = *a;
    }
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

// 由 mode 建 plan（切換 mode 同開機共用）。只在 pair loop／on_start 呼叫。
static esp_err_t content_apply_mode_locked(const nc_px_mode_t *mode)
{
    if (mode == NULL) {
        return ESP_ERR_INVALID_ARG;
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
    build_asset_tbl(mode);
    const nc_px_hw_inv_t inv = {
        .hw = NC_PX_HW_WS2812,
        .base = 0,
        .px = (uint16_t)s_content.px,
    };
    const esp_err_t pe = nc_px_show_plan(mode, map, &s_content.assets, &inv, 1, &s_content.plan);
    if (pe != ESP_OK || s_content.plan.njobs == 0) {
        ESP_LOGW(TAG, "plan 失敗：mode=%s map=%s jobs=%u skipped=%u（%s）",
                 mode->name, map->name, (unsigned)s_content.plan.njobs,
                 (unsigned)s_content.plan.skipped, esp_err_to_name(pe));
        return ESP_ERR_NOT_FOUND;
    }

    // canvas：只在第一次配；切 mode 重用同一塊 px*4。
    if (s_content.canvas == NULL) {
        s_content.canvas = (uint8_t *)heap_caps_malloc((size_t)s_content.px * NC_PX_BPP,
                                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (s_content.canvas == NULL) {
            ESP_LOGW(TAG, "canvas 配置失敗（%u px）", (unsigned)s_content.px);
            return ESP_ERR_NO_MEM;
        }
    }
    memset(s_content.canvas, 0, (size_t)s_content.px * NC_PX_BPP);

    snprintf(s_content.mode_name, sizeof(s_content.mode_name), "%s", mode->name);
    s_content.step = 0;
    s_content.mode_frames = 0;
    s_content.mode_limit = (mode->maxF > 0) ? mode->maxF : 500;
    s_content.ready = true;
    s_content.fallback = false;
    s_last_eff_seq = 0;
    s_plan_gen++;
    s_last_apply_us = (uint32_t)(esp_timer_get_time() - t0);

    ESP_LOGI(TAG, "選中 mode：%s（id=%u）mapping=%s white=%u%%；map=%s（%u groups）",
             mode->name, (unsigned)mode->id,
             (mode->mapping[0] != '\0') ? mode->mapping : "-",
             (unsigned)s_content.white, map->name, (unsigned)map->ngroups);
    ESP_LOGI(TAG, "plan：px=%u jobs=%u skipped=%u；rebuild %u us；環 %s",
             (unsigned)s_content.plan.px, (unsigned)s_content.plan.njobs,
             (unsigned)s_content.plan.skipped, (unsigned)s_last_apply_us,
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
    for (uint8_t i = 0; i < reg->nlist; i++) {
        if (strcmp(reg->list[i], s_content.mode_name) == 0) {
            cur = (int)i;
            break;
        }
    }
    const int start = (cur >= 0) ? (cur + 1) : 0;
    for (uint8_t n = 0; n < reg->nlist; n++) {
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

// effect 經這個 function copy plan；asset 副本 repoint，effect 之後唔再讀 pair 的表。
bool nc_pipe_plan_snapshot(nc_px_plan_t *out, nc_px_asset_t *assets,
                           uint8_t cap, uint32_t *gen)
{
    if (out == NULL || assets == NULL || gen == NULL || s_plan_mtx == NULL) {
        return false;
    }
    if (xSemaphoreTake(s_plan_mtx, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    bool ok = false;
    if (s_content.ready && s_content.plan.njobs > 0 && cap >= s_content.plan.njobs) {
        *out = s_content.plan;
        for (uint8_t j = 0; j < out->njobs; j++) {
            if (out->job[j].asset != NULL) {
                assets[j] = *out->job[j].asset;
                out->job[j].asset = &assets[j];
            }
        }
        *gen = s_plan_gen;
        ok = true;
    }
    xSemaphoreGive(s_plan_mtx);
    return ok;
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
    if (s_content.canvas != NULL) {
        heap_caps_free(s_content.canvas);
        s_content.canvas = NULL;
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
    uint8_t *fb = nc4_decoder_reply_buf(ctx->decoder);
    if (fb == NULL) {
        return;
    }
    uint8_t *data = fb + NC4_HDR_LEN;
    char *js = (char *)(data + 2);               // 2 B = JSON 長度（NC4 回覆格式）
    const size_t cap = NC4_REPLY_JSON_MAX + 1;   // + NUL；回覆幀另有 4 B CRC 餘量
    int n = 0;

    // 快照由 pair loop 更新（另一個任務、可能另一核）→ 短臨界區。
    // hex 直接寫入 decoder 的回覆緩衝，唔喺 decode task 的棧上開大 buffer。
    taskENTER_CRITICAL(&s_content.lock);
    if (!s_content.last_valid) {
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
                         "\"white\":%u,\"apply_us\":%u,\"hex\":\"",
                         (unsigned)px, (unsigned)s_content.last_seq, s_content.mode_name,
                         (unsigned)s_content.white, (unsigned)s_last_apply_us);
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
        .name = "pair",
        .fn = content_loop,
        .stack = 4096,
        .prio = NC_PRIO_OTHERS_MAX - 1,
        .core = 1,
        .layer = 5,
        .period_ms = 0,               // 環滿自己延遲，有空間就填
    },
};

static const nc_task_t s_task = {
    .name = "pair",
    .kind = NC_TASK_NORMAL,
    .level = 4,
    .order = 21,
    .on_start = content_start,
    .on_stop = content_stop,
    .loops = s_loops,
    .nloops = 1,
};

NC_TASK_AUTO_REG(pair, &s_task);

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

// payload [mode_id:u8]；只排隊，pair loop 喺下一幀邊界 rebuild plan。
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

static const nc_action_t s_actions[] = {
    { NC4_CMD_SHOWFRAME, on_showframe, "content" },
    { NC4_CMD_RELOAD, on_reload, "content" },
    { NC4_CMD_SHOWMODE, on_showmode, "content" },
};

NC_ACTION_TABLE_DEF(nc_content_task_table, "pair", s_actions);

#endif  // CONFIG_NETCORE_CONTENT
