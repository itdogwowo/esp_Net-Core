// nc_px_effect_task.c — 四層管線第一段 pixel_effect_task：只生產波形值（layout 版）。
//
// 2026-11-25 四層四任務：
//   **pixel_effect_task**（本檔）→ pixel_paint_task（抖動＋配對）→
//   pixel_encode_task（畫波）→ render_task（公用推送）。
//
// 唔讀 /data、唔碰 map／write／ring；plan（aprog 清單）由 pair 經
// nc_pipe_aprog_publish() 發佈，這裡 copy 一份再自己建 wave table。
//
// 值流按 **asset 去重**：500 個 group 共用 20 個效果 → 只有 20 條值流。
// 兩塊 pool 交替寫、pair 用 ack(seq) 交還，所以 pair 唔會讀到寫到一半的值流。
#include "sdkconfig.h"

#if CONFIG_NETCORE_CONTENT

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_timer.h"

#include "nc_pipeline.h"
#include "nc_px_engine.h"   // nc_px_engine_tables_init（正弦／gamma 表）
#include "nc_prio.h"
#include "nc_px_nce.h"     // .nce 值流信箱
#include "nc_task.h"

static const char *TAG = "px_effect";

static SemaphoreHandle_t s_mtx;
static uint32_t s_gen;                     // 目前生效的 plan gen（0 = 未取過）

// aprog 副本（每份 cap × 536 B）**放 PSRAM**，唔佔 internal。
// 冇固定上限：按 naprog 配置／倍增；只有指標陣列留 internal。
static nc_pipe_aprog_set_t s_set;          // 目前生效的 aprog 副本（own arrays）

// 值流 pool（兩塊交替）；pool[slot] 長度 = Σ nv
static uint16_t *s_pool[2];
static uint32_t s_pool_n[2];
static uint32_t s_slot_len;                // 每塊 pool 的 uint16 數目
static volatile uint32_t s_slot_seq[2];    // 每個 slot 最新寫入的 seq
static volatile uint32_t s_ack;            // pair 已消化的 seq
static uint8_t s_next_slot;
static uint32_t s_seq;
static uint32_t s_step;

static nc_pipe_effect_frame_t s_pub_frame;

// wave table（每條值流一塊；plan 變更時重建）
static uint16_t **s_wave;
static uint32_t *s_wave_len;
static uint32_t s_wave_cap;

// frame 的 view 陣列（effect 擁有；pair 只讀）
static uint16_t **s_frame_vals;
static uint32_t *s_frame_nv;
static uint32_t s_frame_cap;

// aprog 集：pair 是唯一寫者、effect 是唯一讀者。
// 雙緩衝＋一個 volatile 索引，publish 只係換索引（唔使鎖、唔會同效果 loop 爭）。
static nc_pipe_aprog_set_t s_sets[2];      // 2 份發佈緩衝（own arrays，PSRAM）
static volatile uint32_t   s_set_idx;
static volatile uint32_t   s_set_seq;        // 每次 publish +1
static uint32_t            s_set_seen;       // effect 已取到第幾個

// 按需要配置／倍增一份 set 的 nv／aprog（PSRAM）。
static bool set_reserve(nc_pipe_aprog_set_t *t, uint32_t cap)
{
    if (cap == 0) {
        return false;
    }
    if (t->cap >= cap && t->nv != NULL && t->aprog != NULL) {
        return true;
    }
    uint32_t *nv = (uint32_t *)heap_caps_malloc((size_t)cap * sizeof(uint32_t),
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    nc_px_aprog_t *ap = (nc_px_aprog_t *)heap_caps_malloc((size_t)cap * sizeof(nc_px_aprog_t),
                                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (nv == NULL || ap == NULL) {
        heap_caps_free(nv);
        heap_caps_free(ap);
        return false;
    }
    if (t->nv != NULL) {
        heap_caps_free(t->nv);
    }
    if (t->aprog != NULL) {
        heap_caps_free(t->aprog);
    }
    t->nv = nv;
    t->aprog = ap;
    t->cap = cap;
    return true;
}

void nc_pipe_aprog_publish(const nc_px_aprog_t *aprog, uint32_t naprog, uint32_t gen)
{
    if (aprog == NULL || naprog == 0) {
        return;
    }
    const uint32_t slot = (s_set_idx == 0u) ? 1u : 0u;
    nc_pipe_aprog_set_t *dst = &s_sets[slot];
    if (!set_reserve(dst, naprog)) {
        ESP_LOGE(TAG, "aprog set 配置失敗（naprog=%u）", (unsigned)naprog);
        return;
    }
    dst->plan_gen = gen;
    dst->naprog = naprog;
    for (uint32_t i = 0; i < naprog; i++) {
        dst->aprog[i] = aprog[i];
        dst->nv[i] = aprog[i].nv;
    }
    s_set_idx = slot;
    s_set_seq++;
}

bool nc_pipe_aprog_snapshot(nc_pipe_aprog_set_t *out)
{
    if (out == NULL) {
        return false;
    }
    const uint32_t seq = s_set_seq;
    if (seq == 0 || seq == s_set_seen) {
        return false;
    }
    const nc_pipe_aprog_set_t *src = &s_sets[s_set_idx];
    if (!set_reserve(out, src->naprog)) {
        return false;
    }
    out->plan_gen = src->plan_gen;
    out->naprog = src->naprog;
    memcpy(out->nv, src->nv, (size_t)src->naprog * sizeof(uint32_t));
    memcpy(out->aprog, src->aprog, (size_t)src->naprog * sizeof(nc_px_aprog_t));
    s_set_seen = seq;
    return true;
}

static void waves_free(void)
{
    for (uint32_t i = 0; i < s_wave_cap; i++) {
        if (s_wave[i] != NULL) {
            heap_caps_free(s_wave[i]);
            s_wave[i] = NULL;
        }
        s_wave_len[i] = 0;
    }
}

// 指標陣列按 naprog 倍增（internal，細）
static bool wave_slots_reserve(uint32_t cap)
{
    if (cap <= s_wave_cap) {
        return true;
    }
    uint16_t **w = (uint16_t **)heap_caps_malloc((size_t)cap * sizeof(*w),
                                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint32_t *l = (uint32_t *)heap_caps_malloc((size_t)cap * sizeof(*l),
                                               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (w == NULL || l == NULL) {
        heap_caps_free(w);
        heap_caps_free(l);
        return false;
    }
    memset(w, 0, (size_t)cap * sizeof(*w));
    memset(l, 0, (size_t)cap * sizeof(*l));
    for (uint32_t i = 0; i < s_wave_cap; i++) {
        w[i] = s_wave[i];
        l[i] = s_wave_len[i];
    }
    heap_caps_free(s_wave);
    heap_caps_free(s_wave_len);
    s_wave = w;
    s_wave_len = l;
    s_wave_cap = cap;
    return true;
}

static bool waves_build(void)
{
    // ★ 正弦／gamma 表唔係「自動有」：nc_px_engine_sin_lin() 讀的 s_sin_lin[] 預設全 0，
    //   冇 init 就永遠回 2048 → 所有效果都係「中位平線」（燈唔動、phi／F 完全冇作用）。
    //   以前只有舊 8×8 引擎／bench／test_action 會 init，Show 的 content 路徑漏了。
    //   呢個 function 冪等（s_gam[1023] == 0 才建），每次 reload 呼叫都安全。
    nc_px_engine_tables_init();
    waves_free();
    if (!wave_slots_reserve(s_set.naprog)) {
        ESP_LOGW(TAG, "wave 指標陣列配置失敗（naprog=%u）", (unsigned)s_set.naprog);
        return false;
    }
    for (uint32_t i = 0; i < s_set.naprog; i++) {
        const uint32_t total = s_set.aprog[i].prog.total;
        if (total == 0 || total > 8192) {
            continue;
        }
        uint16_t *w = (uint16_t *)heap_caps_malloc(total * sizeof(uint16_t),
                                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (w == NULL) {
            ESP_LOGW(TAG, "wave table OOM（aprog %u, total %u）→ fallback interpreter",
                     (unsigned)i, (unsigned)total);
            continue;
        }
        for (uint32_t p = 0; p < total; p++) {
            w[p] = nc_px_engine_program_value(&s_set.aprog[i].prog, p);
        }
        s_wave[i] = w;
        s_wave_len[i] = total;
    }
    return true;
}

static void pools_free(void)
{
    for (int i = 0; i < 2; i++) {
        if (s_pool[i] != NULL) {
            heap_caps_free(s_pool[i]);
            s_pool[i] = NULL;
        }
        s_pool_n[i] = 0;
        s_slot_seq[i] = 0;
    }
    s_slot_len = 0;
}

static bool pools_alloc(uint32_t total)
{
    pools_free();
    if (total == 0) {
        return false;
    }
    // 政策：值流細（每塊 ≤48 KB）就放 internal（pair apply 讀得快）；
    // 大過就 PSRAM。兩個 pool 都要放得落先會用 internal。
    const size_t pool_bytes = (size_t)total * sizeof(uint16_t);
    const bool want_internal = (pool_bytes <= 49152u);
    for (int i = 0; i < 2; i++) {
        if (want_internal) {
            s_pool[i] = (uint16_t *)heap_caps_malloc(pool_bytes,
                                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
        if (s_pool[i] == NULL) {
            s_pool[i] = (uint16_t *)heap_caps_malloc(pool_bytes,
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
        if (s_pool[i] == NULL && !want_internal) {
            s_pool[i] = (uint16_t *)heap_caps_malloc(pool_bytes,
                                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
        if (s_pool[i] == NULL) {
            pools_free();
            return false;
        }
        s_pool_n[i] = total;
    }
    s_slot_len = total;
    s_next_slot = 0;
    s_slot_seq[0] = 0;
    s_slot_seq[1] = 0;
    s_ack = 0;
    return true;
}

bool nc_pipe_effect_copy(nc_pipe_effect_frame_t *out)
{
    if (out == NULL || s_mtx == NULL) {
        return false;
    }
    if (xSemaphoreTake(s_mtx, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    const bool ok = (s_seq != 0);
    if (ok) {
        *out = s_pub_frame;
    }
    xSemaphoreGive(s_mtx);
    return ok;
}

void nc_pipe_effect_ack(uint32_t seq)
{
    if (seq > s_ack) {
        s_ack = seq;
    }
}

// 等一個「pair 已消化」的 slot（pair 慢就等，唔會撕裂值流）
static bool wait_slot(uint8_t slot)
{
    if (s_slot_seq[slot] == 0 || s_slot_seq[slot] <= s_ack) {
        return true;
    }
    return false;
}

// 用預算好的 wave table 算一條值流（同 nc_px_aprog_vals 等價）
static uint32_t cached_vals(uint32_t i, uint32_t step, uint16_t *out, uint32_t cap)
{
    const nc_px_aprog_t *ap = &s_set.aprog[i];
    const nc_px_asset_t *a = ap->asset;
    if (a == NULL || out == NULL) {
        return 0;
    }
    uint32_t n = ap->nv;
    if (n > cap) {
        n = cap;
    }
    // `.nce`：值流唔係畫波，而係插件每 tick 填好的信箱（0–255 → <<4 入 12-bit）。
    // 未 publish（插件未開／未跑）→ 全 0（黑），唔會讀到亂數。
    if (a->kind == NC_PX_ASSET_NCE) {
        nc_px_nce_frame_t f;
        if (!nc_px_nce_latest(&f) || f.data == NULL) {
            memset(out, 0, (size_t)n * sizeof(uint16_t));
            return n;
        }
        const uint32_t k = nc_px_nce_to_vals(f.data, f.bytes, out, n);
        for (uint32_t j = k; j < n; j++) {
            out[j] = 0;
        }
        return n;
    }
    if (s_wave[i] == NULL) {
        return nc_px_aprog_vals(ap, step, out, n);
    }
    const uint32_t speed = (a->speed == 0) ? 1 : a->speed;
    uint32_t ph = ((step / speed) * (uint32_t)a->step + (uint32_t)a->offset);
    const uint32_t total = s_wave_len[i];
    ph = total ? (ph % total) : 0;
    for (uint32_t k = 0; k < n; k++) {
        const uint32_t idx = a->reverse ? (n - 1u - k) : k;
        uint32_t p = ph + idx * (uint32_t)a->spacing;
        p = total ? (p % total) : 0;
        out[k] = s_wave[i][p];
    }
    return n;
}

static void effect_loop(void *arg)
{
    (void)arg;
    const uint32_t fps = (CONFIG_NETCORE_PIXEL_FPS > 0) ? CONFIG_NETCORE_PIXEL_FPS : 50;
    const TickType_t period = pdMS_TO_TICKS(1000u / fps);
    TickType_t last = xTaskGetTickCount();

    if (s_mtx == NULL) {
        s_mtx = xSemaphoreCreateMutex();
    }
    while (!nc_task_exit_requested()) {
        // snapshot 直接寫入工作副本（PSRAM；唔喺棧上開 17 KB）
        if (nc_pipe_aprog_snapshot(&s_set)) {
            s_gen = s_set.plan_gen;
            s_step = 0;
            waves_build();
            uint32_t total = 0;
            for (uint32_t i = 0; i < s_set.naprog; i++) {
                total += s_set.nv[i];
            }
            if (!pools_alloc(total)) {
                ESP_LOGE(TAG, "值流 pool 配置失敗（%u 個 uint16）", (unsigned)total);
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            ESP_LOGI(TAG, "plan gen=%u：%u 條值流（%u 值、pool ×2）",
                     (unsigned)s_gen, (unsigned)s_set.naprog, (unsigned)total);
        } else if (s_gen == 0) {
            nc_task_beat();
            vTaskDelay(pdMS_TO_TICKS(10));      // pair 未發佈 aprog
            last = xTaskGetTickCount();
            continue;
        }

        // 揀一個 pair 已消化的 slot（另一塊正在被讀）
        const uint8_t slot = s_next_slot;
        if (!wait_slot(slot)) {
            nc_task_beat();
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        const int64_t t0 = esp_timer_get_time();
        uint32_t off = 0;
        for (uint32_t i = 0; i < s_set.naprog; i++) {
            const uint32_t nv = s_set.nv[i];
            cached_vals(i, s_step, s_pool[slot] + off, nv);
            off += nv;
        }
        const int64_t t1 = esp_timer_get_time();
        nc_pipe_stats_effect((uint32_t)(t1 - t0));
        s_step++;

        if (xSemaphoreTake(s_mtx, portMAX_DELAY) == pdTRUE) {
            uint32_t o = 0;
            s_pub_frame.seq = ++s_seq;
            s_pub_frame.plan_gen = s_gen;
            s_pub_frame.t_pub_us = (uint32_t)t1;
            if (s_set.naprog > s_frame_cap) {
                uint32_t *nv = (uint32_t *)heap_caps_malloc(
                    (size_t)s_set.naprog * sizeof(uint32_t),
                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
                uint16_t **vv = (uint16_t **)heap_caps_malloc(
                    (size_t)s_set.naprog * sizeof(uint16_t *),
                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
                if (nv != NULL && vv != NULL) {
                    heap_caps_free(s_frame_nv);
                    heap_caps_free(s_frame_vals);
                    s_frame_nv = nv;
                    s_frame_vals = vv;
                    s_frame_cap = s_set.naprog;
                } else {
                    heap_caps_free(nv);
                    heap_caps_free(vv);
                }
            }
            s_pub_frame.naprog = s_set.naprog;
            s_pub_frame.nv = s_frame_nv;
            s_pub_frame.vals = s_frame_vals;
            for (uint32_t i = 0; i < s_set.naprog && i < s_frame_cap; i++) {
                s_frame_nv[i] = s_set.nv[i];
                s_frame_vals[i] = s_pool[slot] + o;
                o += s_set.nv[i];
            }
            s_slot_seq[slot] = s_pub_frame.seq;
            s_next_slot = (uint8_t)(slot ^ 1u);
            xSemaphoreGive(s_mtx);
        }
        nc_task_beat();
        if (nc_pipe_stats_burn_active()) {
            taskYIELD();                  // 極限：唔等 50 fps
        } else {
            vTaskDelayUntil(&last, period);
        }
    }
    pools_free();
    waves_free();
}

static const nc_task_loop_t s_loops[] = {
    {
        .name = "effect",
        .fn = effect_loop,
        .stack = 4096,
        .prio = NC_PRIO_OTHERS_MAX - 1,
        .core = 0,
        .layer = 5,
        .period_ms = 0,
    },
};

static const nc_task_t s_task = {
    .name = "pixel_effect_task",      // 2026-11-25 正名（tasks.json 舊名 effect 由 alias 讀）
    .kind = NC_TASK_NORMAL,
    .level = 4,
    .order = 22,
    .on_start = NULL,
    .on_stop = NULL,
    .loops = s_loops,
    .nloops = 1,
};

NC_TASK_AUTO_REG(pixel_effect_task, &s_task);

#endif  // CONFIG_NETCORE_CONTENT
