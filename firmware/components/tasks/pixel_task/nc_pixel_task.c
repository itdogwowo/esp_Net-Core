// nc_pixel_task.c — 效果／Map 生產者任務（pixel）。
//
// 只做一件事：按自己的節奏（預設 60 fps）算一格 canonical R,G,B,W，寫進環。
// **唔送燈**：送燈係 render 任務（tasks/render_task）的事；兩者只透過環見面。
//
// 環係 render 任務宣告／建立／登記的（名字例如 "i80.prd"）；這個任務在 on_start
// 時按名字（含 "prd"）找回它。所以啟動次序：render 先、pixel 後 —— main.c 依這個
// 次序登記，任務管理器亦依登記次序跑 on_start。
#include "nc_pixel_task.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nc_pixel_core.h"
#include "nc_pixel_effects.h"
#include "nc_px_engine.h"
#include "nc_prio.h"
#include "sdkconfig.h"

#if CONFIG_NETCORE_PIXEL && !CONFIG_NETCORE_CONTENT

static const char *TAG = "pixel";

static struct {
    bool          started;
    nc_pixel_q_t *q;            // 由 render 任務建立、這裡只負責寫
    uint32_t      busy;         // 環滿而跳過的拍數
    uint32_t      frames;       // 成功提交的格數
    bool          defaults_done;
} s_prd;

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

// 算一格並提交。**不控 FPS**：有空間就填，填滿就等 renderer 空出位置。
// 效果是時間的函數，所以同一毫秒內重複算出來的畫面一樣；環一滿自然被 renderer
// 的耗用速度限流（平均生產率 = 渲染率），唔需要生產者自己計節拍。
static bool producer_tick(int64_t now_us)
{
    if (s_prd.q == NULL || !nc_px_engine_ready()) {
        return false;
    }
    nc_px_hdr_t *hdr = NULL;
    uint8_t *p = nc_px_ring_begin_ex(s_prd.q, &hdr);
    if (p == NULL) {
        return false;                     // 環滿
    }
    if (!nc_px_engine_frame(p, hdr, now_us)) {
        nc_px_ring_abort(s_prd.q);
        return false;
    }
    nc_px_ring_commit(s_prd.q);
    s_prd.frames++;
    return true;
}

// ── 生產者 loop：有空間就瘋狂填，填滿就進入「監控」等 renderer 還格。
// 唔控制 FPS（渲染節奏由 render 任務讀 system.json 的 frame_interval_ms 決定）。
static void prd_loop(void *arg)
{
    (void)arg;
    while (!nc_task_exit_requested()) {
        if (s_prd.q == NULL) {
            nc_task_beat();
            vTaskDelay(pdMS_TO_TICKS(100));       // 沒有環（render 未起）→ 閒著等
            continue;
        }
        const int64_t now = esp_timer_get_time();
        if (!producer_tick(now)) {
            // 環滿：監控狀態。1 ms 看一次 renderer 有沒有還格（renderer 由
            // frame_interval_ms 控制，通常 20 ms 一格；1 ms 粒度足夠）。
            s_prd.busy++;
            nc_task_beat();
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        nc_task_beat();
        taskYIELD();                          // 讓 renderer（更高 prio）先跑
    }
}

static esp_err_t prd_start(const nc_task_res_view_t *res, int nres)
{
    (void)res;
    (void)nres;
    // 內建效果交去引擎 registry（冪等；引擎唔知道有邊幾個效果，只認 id）。
    nc_pixel_effects_register();
    // 預設值立一次（效果／亮度／位數／demo）。生產者 fps 不再參與節奏（0 = 自由填環）。
    if (!s_prd.defaults_done) {
        nc_px_engine_defaults();
        s_prd.defaults_done = true;
    }
    s_prd.q = find_prd_ring();
    if (s_prd.q == NULL) {
        // 不是錯誤：可能這塊板只跑 render（主機推送）或者 render 未啟動。
        ESP_LOGW(TAG, "找不到 prd 環 → 效果生產者不啟動（render 任務有起嗎？）");
        return ESP_OK;
    }
    const esp_err_t e = nc_px_engine_bind(nc_px_ring_px(s_prd.q));
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "綁定 prd 環失敗：%s", esp_err_to_name(e));
        s_prd.q = NULL;
        return e;
    }
    memset(&s_prd.busy, 0, sizeof(s_prd.busy));
    s_prd.frames = 0;
    s_prd.started = true;
    ESP_LOGI(TAG, "效果生產者就緒：環 %s、%lu 顆、%u fps",
             nc_px_ring_name(s_prd.q), (unsigned long)nc_px_ring_px(s_prd.q),
             (unsigned)nc_px_engine_cfg().fps);
    return ESP_OK;
}

static void prd_stop(void)
{
    s_prd.started = false;
    s_prd.q = NULL;
}

static const nc_task_loop_t s_loops[] = {
    {
        // 生產者：效果 → 環。優先級低 render 一級，兩者同核時送燈先過。
        .name = "prd",
        .fn = prd_loop,
        .stack = 3072,                // 效果只做算術＋寫一格，唔使 renderer 咁多
        .prio = NC_PRIO_OTHERS_MAX - 1,
        .core = CONFIG_NETCORE_PIXEL_CORE,
        .layer = 5,
        .period_ms = 0,
    },
};

static const nc_task_t s_task = {
    .name = "pixel",
    .kind = NC_TASK_NORMAL,
    .level = 4,
    .order = 20,
    .on_start = prd_start,
    .on_stop = prd_stop,
    .loops = s_loops,
    .nloops = 1,
};

NC_TASK_AUTO_REG(pixel, &s_task);

const nc_task_t *nc_pixel_task(void)
{
    return &s_task;
}

#else   // !CONFIG_NETCORE_PIXEL || CONFIG_NETCORE_CONTENT

const nc_task_t *nc_pixel_task(void)
{
    return NULL;
}

#endif  // CONFIG_NETCORE_PIXEL && !CONFIG_NETCORE_CONTENT
