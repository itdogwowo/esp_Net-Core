// GPIO owner（nc_hw resource "gpio"）
//
// 唯一可以 gpio_config / gpio_set_level 的地方；UI／NC4 action 只可以：
//   nc_hw_send("gpio", {CFG/SET...})  → owner 執行
//   nc_hw_snapshot("gpio", ...)       → 讀最新 levels
#pragma once

#include "nc_task.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NC_HW_GPIO_NAME "gpio"

typedef enum {
    NC_HW_GPIO_CFG = 0,   // pin, arg=mode(0 out/1 in), data[0]=pull(0/1/2), data[1]=level
    NC_HW_GPIO_SET = 1,   // pin, arg=level
} nc_hw_gpio_op_t;

typedef struct {
    uint64_t levels;      // bit n = pin n 的目前電平
    uint8_t  mode[49];    // 0=out, 1=in, 0xFF=未配置
} nc_hw_gpio_snap_t;

const nc_task_t *nc_hw_gpio_task(void);

#ifdef __cplusplus
}
#endif
