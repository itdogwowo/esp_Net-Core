// PWM（LEDC）驅動 —— 照舊專案 PWM.list：一個 GPIO 一個通道。
//
// 可選 freq／resolution／duty；舊專案只有 GPIO（freq=1000、duty=0）。
// 多個通道可以共用同一個 timer（同 freq/resolution）；不同設定會用下一個 timer。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NC_PWM_DRV_MAX 8

typedef struct {
    int      pin;             // >= 0
    uint32_t freq_hz;         // default 1000
    uint8_t  resolution_bits; // 1..14
    uint16_t duty;            // 0..(2^bits-1)；預設 12-bit = 0..4095
    uint16_t d_stay;          // 停止／歸零要回到的中性值（舊專案 dStay）
} nc_pwm_drv_cfg_t;

esp_err_t nc_pwm_drv_install(const nc_pwm_drv_cfg_t *cfg, int n);
int       nc_pwm_drv_count(void);
bool      nc_pwm_drv_ready(void);
esp_err_t nc_pwm_drv_set(int idx, uint32_t duty);
// 停止／歸零：回到 config 的 dStay（舊專案中性值；燈 0、motor 2048）。
esp_err_t nc_pwm_drv_stay(int idx);
// 真正數值 0（不是 dStay）：明確要求 0 時用。
esp_err_t nc_pwm_drv_zero(int idx);
uint32_t  nc_pwm_drv_get(int idx);

#ifdef __cplusplus
}
#endif
