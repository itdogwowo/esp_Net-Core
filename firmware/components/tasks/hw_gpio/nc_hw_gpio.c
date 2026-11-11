#include "nc_hw_gpio.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "nc_cfg.h"
#include "nc_hw.h"
#include "nc_prio.h"

static const char *TAG = "hw_gpio";
static nc_hw_gpio_snap_t s_snap;

static bool pin_ok(int pin)
{
    return pin >= 0 && pin <= 48 && !nc_hw_reserved(pin) &&
           !nc_hw_pin_claimed_by_other(pin, NC_HW_GPIO_NAME);
}

static void publish(void)
{
    (void)nc_hw_publish(NC_HW_GPIO_NAME, &s_snap, sizeof(s_snap));
}

static esp_err_t cfg_pin(int pin, int mode, int pull, int level)
{
    if (!pin_ok(pin)) {
        return ESP_ERR_INVALID_ARG;
    }
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << pin),
        .mode = mode ? GPIO_MODE_INPUT : GPIO_MODE_OUTPUT,
        .pull_up_en = (pull == 1) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = (pull == 2) ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    const esp_err_t e = gpio_config(&io);
    if (e != ESP_OK) {
        return e;
    }
    s_snap.mode[pin] = (uint8_t)mode;
    if (mode == 0) {
        gpio_set_level(pin, level ? 1 : 0);
        if (level) {
            s_snap.levels |= (1ULL << pin);
        } else {
            s_snap.levels &= ~(1ULL << pin);
        }
    }
    return ESP_OK;
}

static void sample_inputs(void)
{
    for (int pin = 0; pin <= 48; pin++) {
        if (s_snap.mode[pin] != 1) {
            continue;
        }
        const int v = gpio_get_level(pin) ? 1 : 0;
        if (v) {
            s_snap.levels |= (1ULL << pin);
        } else {
            s_snap.levels &= ~(1ULL << pin);
        }
    }
}

static void gpio_loop(void *arg)
{
    (void)arg;
    for (;;) {
        nc_task_beat();
        nc_hw_cmd_t cmd;
        if (nc_hw_recv(NC_HW_GPIO_NAME, &cmd, 10)) {
            if (cmd.op == NC_HW_GPIO_CFG) {
                const esp_err_t e = cfg_pin(cmd.pin, cmd.arg, cmd.data[0], cmd.data[1]);
                if (e != ESP_OK) {
                    ESP_LOGW(TAG, "CFG pin=%u mode=%u → %s", cmd.pin, cmd.arg,
                             esp_err_to_name(e));
                }
            } else if (cmd.op == NC_HW_GPIO_SET) {
                if (!pin_ok(cmd.pin)) {
                    ESP_LOGW(TAG, "SET pin=%u 被拒（保留／越界）", cmd.pin);
                } else {
                    if (s_snap.mode[cmd.pin] != 0) {
                        (void)cfg_pin(cmd.pin, 0, 0, 0);
                    }
                    gpio_set_level(cmd.pin, cmd.arg ? 1 : 0);
                    if (cmd.arg) {
                        s_snap.levels |= (1ULL << cmd.pin);
                    } else {
                        s_snap.levels &= ~(1ULL << cmd.pin);
                    }
                }
            } else {
                ESP_LOGW(TAG, "未知 op=%u", cmd.op);
            }
        }
        sample_inputs();
        publish();
    }
}

static esp_err_t on_start(const nc_task_res_view_t *res, int nres)
{
    (void)res;
    (void)nres;
    memset(&s_snap, 0, sizeof(s_snap));
    memset(s_snap.mode, 0xFF, sizeof(s_snap.mode));
    const esp_err_t e = nc_hw_register(NC_HW_GPIO_NAME);
    if (e != ESP_OK) {
        return e;
    }
    // hardware.json 的 PIN.list：由 owner 套用（唔喺 main 直接 gpio_config）。
    const nc_hardware_cfg_t *hw = nc_cfg_hardware();
    if (hw != NULL) {
        for (uint8_t i = 0; i < hw->n_pin; i++) {
            const esp_err_t ce = cfg_pin(hw->pin[i].pin, hw->pin[i].mode,
                                         hw->pin[i].pull, hw->pin[i].initial);
            if (ce != ESP_OK) {
                ESP_LOGW(TAG, "PIN %d（%s）→ %s", hw->pin[i].pin, hw->pin[i].label,
                         esp_err_to_name(ce));
            }
        }
    }
    publish();
    return ESP_OK;
}

static const nc_task_loop_t s_loops[] = {
    {
        .name = "hw_gpio",
        .fn = gpio_loop,
        .stack = 3072,
        .prio = NC_PRIO_OTHERS_MAX,
        .core = 0,
        .layer = 2,
        .period_ms = 0,
    },
};

static const nc_task_t s_task = {
    .name = "hw_gpio",
    .kind = NC_TASK_SYSTEM,
    .level = 2,
    .loops = s_loops,
    .nloops = 1,
    .on_start = on_start,
};

const nc_task_t *nc_hw_gpio_task(void)
{
    return &s_task;
}
