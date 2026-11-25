// nc_pipe_stats.c — 三任務的分段計時（effect／pair／apply／white／commit／latency）。
#include "sdkconfig.h"

#if CONFIG_NETCORE_CONTENT

#include "nc_pipeline.h"

#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static nc_pipe_stats_t s_st;
static uint64_t s_eff_total, s_pair_total, s_apply_total;
static uint64_t s_clear_total;
static uint64_t s_white_total, s_commit_total, s_lat_total;
static uint32_t s_full;
static volatile bool s_burn;
static volatile uint32_t s_burn_until_us;

static uint32_t avg(uint64_t total, uint32_t n)
{
    return n ? (uint32_t)(total / n) : 0;
}

void nc_pipe_stats_reset(void)
{
    taskENTER_CRITICAL(&s_lock);
    memset(&s_st, 0, sizeof(s_st));
    s_eff_total = s_pair_total = s_apply_total = 0;
    s_clear_total = s_white_total = s_commit_total = s_lat_total = 0;
    s_full = 0;
    taskEXIT_CRITICAL(&s_lock);
}

void nc_pipe_stats_get(nc_pipe_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    taskENTER_CRITICAL(&s_lock);
    *out = s_st;
    out->eff_avg_us = avg(s_eff_total, s_st.eff_n);
    out->pair_avg_us = avg(s_pair_total, s_st.pair_n);
    out->clear_avg_us = avg(s_clear_total, s_st.pair_n);
    out->apply_avg_us = avg(s_apply_total, s_st.pair_n);
    out->white_avg_us = avg(s_white_total, s_st.pair_n);
    out->commit_avg_us = avg(s_commit_total, s_st.pair_n);
    out->lat_avg_us = avg(s_lat_total, s_st.pair_n);
    out->full = s_full;
    taskEXIT_CRITICAL(&s_lock);
}

void nc_pipe_stats_effect(uint32_t us)
{
    taskENTER_CRITICAL(&s_lock);
    s_st.eff_n++;
    s_st.eff_last_us = us;
    if (us > s_st.eff_max_us) {
        s_st.eff_max_us = us;
    }
    s_eff_total += us;
    taskEXIT_CRITICAL(&s_lock);
}

void nc_pipe_stats_pair(uint32_t clear_us, uint32_t apply_us, uint32_t white_us,
                        uint32_t commit_us, uint32_t lat_us)
{
    taskENTER_CRITICAL(&s_lock);
    s_st.pair_n++;
    s_st.clear_last_us = clear_us;
    s_st.apply_last_us = apply_us;
    s_st.white_last_us = white_us;
    s_st.commit_last_us = commit_us;
    s_st.lat_last_us = lat_us;
    s_st.pair_last_us = clear_us + apply_us + white_us + commit_us;
    if (clear_us > s_st.clear_max_us) s_st.clear_max_us = clear_us;
    if (apply_us > s_st.apply_max_us) s_st.apply_max_us = apply_us;
    if (white_us > s_st.white_max_us) s_st.white_max_us = white_us;
    if (commit_us > s_st.commit_max_us) s_st.commit_max_us = commit_us;
    if (lat_us > s_st.lat_max_us) s_st.lat_max_us = lat_us;
    if (s_st.pair_last_us > s_st.pair_max_us) s_st.pair_max_us = s_st.pair_last_us;
    s_clear_total += clear_us;
    s_pair_total += s_st.pair_last_us;
    s_apply_total += apply_us;
    s_white_total += white_us;
    s_commit_total += commit_us;
    s_lat_total += lat_us;
    taskEXIT_CRITICAL(&s_lock);
}

void nc_pipe_stats_full(void)
{
    taskENTER_CRITICAL(&s_lock);
    s_full++;
    taskEXIT_CRITICAL(&s_lock);
}

void nc_pipe_stats_burn(uint32_t seconds)
{
    taskENTER_CRITICAL(&s_lock);
    if (seconds == 0) {
        s_burn = false;
        s_burn_until_us = 0;
    } else {
        s_burn = true;
        s_burn_until_us = (uint32_t)(esp_timer_get_time() / 1000) + seconds * 1000u;
    }
    taskEXIT_CRITICAL(&s_lock);
}

bool nc_pipe_stats_burn_active(void)
{
    if (!s_burn) {
        return false;
    }
    if ((uint32_t)(esp_timer_get_time() / 1000) >= s_burn_until_us) {
        s_burn = false;
        return false;
    }
    return true;
}

#endif
