#include "nc_pwm_drv.h"

#include <string.h>

#include "driver/ledc.h"
#include "esp_log.h"

static const char *TAG = "pwm_drv";

typedef struct {
    bool     used;
    int      pin;
    int      channel;
    int      timer;
    uint8_t  bits;
    uint32_t freq;
    uint32_t duty;
    uint32_t stay;
} pwm_slot_t;

static pwm_slot_t s_slot[NC_PWM_DRV_MAX];
static int s_count;

typedef struct {
    bool     used;
    uint32_t freq;
    uint8_t  bits;
} pwm_timer_t;

static pwm_timer_t s_timer[LEDC_TIMER_MAX];

bool nc_pwm_drv_ready(void)
{
    return s_count > 0;
}

int nc_pwm_drv_count(void)
{
    return s_count;
}

esp_err_t nc_pwm_drv_install(const nc_pwm_drv_cfg_t *cfg, int n)
{
    if (cfg == NULL || n < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_count > 0) {
        return ESP_OK;
    }
    int timers = 0;
    int channels = 0;
    for (int i = 0; i < n && s_count < NC_PWM_DRV_MAX; i++) {
        const nc_pwm_drv_cfg_t *c = &cfg[i];
        if (c->pin < 0) {
            continue;
        }
        uint32_t freq = (c->freq_hz > 0) ? c->freq_hz : 1000;
        uint8_t bits = (c->resolution_bits >= 1 && c->resolution_bits <= 14)
                           ? c->resolution_bits : 12;
        int timer = -1;
        for (int t = 0; t < timers; t++) {
            if (s_timer[t].used && s_timer[t].freq == freq && s_timer[t].bits == bits) {
                timer = t;
                break;
            }
        }
        if (timer < 0) {
            if (timers >= LEDC_TIMER_MAX) {
                ESP_LOGE(TAG, "timer 不夠（%d）→ pin %d 跳過", LEDC_TIMER_MAX, c->pin);
                continue;
            }
            timer = timers++;
            s_timer[timer].used = true;
            s_timer[timer].freq = freq;
            s_timer[timer].bits = bits;
            ledc_timer_config_t tc = {
                .speed_mode = LEDC_LOW_SPEED_MODE,
                .duty_resolution = (ledc_timer_bit_t)bits,
                .timer_num = (ledc_timer_t)timer,
                .freq_hz = freq,
                .clk_cfg = LEDC_AUTO_CLK,
            };
            const esp_err_t e = ledc_timer_config(&tc);
            if (e != ESP_OK) {
                ESP_LOGE(TAG, "timer %d 設定失敗 %s", timer, esp_err_to_name(e));
                s_timer[timer].used = false;
                timers--;
                continue;
            }
        }
        if (channels >= LEDC_CHANNEL_MAX) {
            ESP_LOGE(TAG, "channel 不夠 → pin %d 跳過", c->pin);
            continue;
        }
        const int ch = channels++;
        const uint32_t max_duty = (1u << bits) - 1u;
        const uint32_t duty = (c->duty > max_duty) ? max_duty : c->duty;
        ledc_channel_config_t cc = {
            .gpio_num = c->pin,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = (ledc_channel_t)ch,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = (ledc_timer_t)timer,
            .duty = duty,
            .hpoint = 0,      // 起始位置歸零：每個週期由 timer zero 開始
        };
        const esp_err_t e = ledc_channel_config(&cc);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "channel %d（pin %d）設定失敗 %s", ch, c->pin, esp_err_to_name(e));
            channels--;
            continue;
        }
        s_slot[s_count].used = true;
        s_slot[s_count].pin = c->pin;
        s_slot[s_count].channel = ch;
        s_slot[s_count].timer = timer;
        s_slot[s_count].bits = bits;
        s_slot[s_count].freq = freq;
        s_slot[s_count].duty = duty;
        s_slot[s_count].stay = (c->d_stay > max_duty) ? max_duty : c->d_stay;
        s_count++;
    }
    ESP_LOGW(TAG, "PWM：%d 通道（%d timer）", s_count, timers);
    return (s_count > 0) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t nc_pwm_drv_set(int idx, uint32_t duty)
{
    if (idx < 0 || idx >= s_count || !s_slot[idx].used) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint32_t max_duty = (1u << s_slot[idx].bits) - 1u;
    if (duty > max_duty) {
        duty = max_duty;
    }
    const esp_err_t e = ledc_set_duty(LEDC_LOW_SPEED_MODE,
                                      (ledc_channel_t)s_slot[idx].channel, duty);
    if (e == ESP_OK) {
        (void)ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)s_slot[idx].channel);
        s_slot[idx].duty = duty;
    }
    return e;
}

esp_err_t nc_pwm_drv_stay(int idx)
{
    if (idx < 0 || idx >= s_count || !s_slot[idx].used) {
        return ESP_ERR_INVALID_ARG;
    }
    return nc_pwm_drv_set(idx, s_slot[idx].stay);
}

esp_err_t nc_pwm_drv_zero(int idx)
{
    return nc_pwm_drv_set(idx, 0);
}

uint32_t nc_pwm_drv_get(int idx)
{
    if (idx < 0 || idx >= s_count || !s_slot[idx].used) {
        return 0;
    }
    return s_slot[idx].duty;
}
