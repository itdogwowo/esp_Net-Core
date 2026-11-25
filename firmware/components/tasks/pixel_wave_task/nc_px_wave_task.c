// nc_px_wave_task.c — pixel 管線第三層：畫波（pixel_wave_task）。
//
// 2026-11-25 四層四任務：
//   pixel_effect_task（效果值）→ pixel_pair_task（抖動＋配對）→
//   **pixel_wave_task（本檔；環 → codec → 後端 DMA 槽）** → render_task（公用推送）。
//
// 為什麼畫波同推送分開：
//   · 畫波係 CPU 重活（9,600 px 合成 ~6.4 ms），推送係 submit＋等 DMA；
//   · 分開之後各自可以配核心／優先級，push 唔會被畫波阻住；
//   · 兩者只傳「格索引」隊列（8 格），唔 copy 任何像素。
//
// 本檔只擁有 loop／節拍／生命週期；環、lane、後端、定時器由 render_task 擁有
// （render 係公用輸出層，唔屬 pixel 系列）。實際 encode 經 nc_render_wave_tick()。
#include "sdkconfig.h"

#if CONFIG_NETCORE_PIXEL

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nc_prio.h"
#include "nc_render.h"
#include "nc_task.h"

static void wave_loop(void *arg)
{
    (void)arg;
    while (!nc_task_exit_requested()) {
        // render_task 未 on_start（或者被 config 關掉）→ 冇環／冇 queue，等它。
        if (!nc_render_wave_ready()) {
            vTaskDelay(pdMS_TO_TICKS(100));
            nc_task_beat();
            continue;
        }
        // 節拍由 render 的 esp_timer 給（system.json 的 frame_interval_ms）。
        // timeout 200 ms：正常 20 ms 一個節拍，停止請求唔會等太久。
        (void)nc_render_wave_wait(200);
        nc_render_wave_tick();
        nc_task_beat();
    }
}

static const nc_task_loop_t s_loops[] = {
    {
        // 畫波：環 → 編碼 → 後端 DMA 槽（只入隊，唔 submit）。
        .name = "wave",
        .fn = wave_loop,
        .stack = CONFIG_NETCORE_PIXEL_STACK,
        .prio = NC_PRIO_OTHERS_MAX,   // 租戶上限；寫更高會被管理器夾回來
        .core = CONFIG_NETCORE_PIXEL_CORE,
        .layer = 5,
        .period_ms = 0,               // 節拍由 render 的 timer 給
    },
};

static const nc_task_t s_task = {
    .name = "pixel_wave_task",
    .kind = NC_TASK_NORMAL,
    .level = 4,
    .order = 11,                      // render_task order 10 先 on_start（環／節拍已就緒）
    .loops = s_loops,
    .nloops = 1,
};

NC_TASK_AUTO_REG(pixel_wave_task, &s_task);

#else   // !CONFIG_NETCORE_PIXEL

// 這塊板沒開 pixel：component 是空 object，唔登記任何任務。

#endif
