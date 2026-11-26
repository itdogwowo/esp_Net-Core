// 開機 sync state.NCSTATE：由 config 派生（P2 第一步）。
#include "nc_state_boot.h"

#include <string.h>

#include "esp_log.h"
#include "nc_cfg.h"
#include "nc_state_blob.h"
#include "nc_state_file.h"

static const char *TAG = "state";

// 來源 JSON（次序固定；hash 跟次序，改次序＝版本變）
static const char *const k_src_paths[] = {
    "/data/config/hardware.json",
    "/data/config/driver.json",
    "/data/config/system.json",
    "/data/config/tasks.json",
    "/data/config/resources.json",
};

// key id（同 docs/03_notes/state-storage-format.md 登記）
enum {
    K_SYS_FRAME_MS   = 0x0002,
    K_SYS_NUM_PIXELS = 0x0003,
    K_SYS_LOG_DEBUG  = 0x0004,
    K_I80_COUNT      = 0x0701,
    K_I80_WR         = 0x0702,
    K_I80_DC         = 0x0703,
    K_I80_CS         = 0x0704,
    K_I80_FREQ       = 0x0705,
    K_LED_ENABLE     = 0x0C01,
    K_LED_BACKEND    = 0x0C02,
    K_LED_BUS        = 0x0C03,
    K_LED_LANES      = 0x0C04,
    K_LED_PX         = 0x0C05,
    K_LED_FMT        = 0x0C06,
    K_LED_CLK        = 0x0C07,
    K_LED_BIT_CELLS  = 0x0C08,
    K_LED_T0H        = 0x0C09,
    K_LED_T1H        = 0x0C0A,
};

static nc_state_rec_t s_recs[32];

static size_t put_u(size_t n, uint16_t key, uint32_t v)
{
    s_recs[n] = (nc_state_rec_t){ .key = key, .type = NC_STATE_T_UINT, .u = v };
    return n + 1;
}

static size_t put_i(size_t n, uint16_t key, int32_t v)
{
    s_recs[n] = (nc_state_rec_t){ .key = key, .type = NC_STATE_T_SINT, .i = v };
    return n + 1;
}

static size_t put_b(size_t n, uint16_t key, bool v)
{
    s_recs[n] = (nc_state_rec_t){ .key = key, .type = NC_STATE_T_BOOL, .u = v ? 1u : 0u };
    return n + 1;
}

static size_t put_s(size_t n, uint16_t key, const char *s)
{
    s_recs[n] = (nc_state_rec_t){ .key = key, .type = NC_STATE_T_BYTES,
                                  .bytes = s, .bytes_len = (uint16_t)strlen(s) };
    return n + 1;
}

// 由已 parse 的 cfg 造 record（sparse：只寫真的值；default 留給 schema／L0）
static size_t build_records(void)
{
    size_t n = 0;

    const nc_system_cfg_t *sys = nc_cfg_system();
    if (sys != NULL) {
        n = put_u(n, K_SYS_FRAME_MS, sys->frame_interval_ms);
        n = put_u(n, K_SYS_NUM_PIXELS, sys->num_pixels);
        n = put_u(n, K_SYS_LOG_DEBUG, sys->log_debug_mode);
    }

    const nc_hardware_cfg_t *hw = nc_cfg_hardware();
    if (hw != NULL && hw->n_i80 > 0) {
        const nc_i80_cfg_t *b = &hw->i80[0];
        n = put_u(n, K_I80_COUNT, b->n_used);
        n = put_i(n, K_I80_WR, b->wr);
        n = put_i(n, K_I80_DC, b->dc);
        n = put_i(n, K_I80_CS, b->cs);
        n = put_u(n, K_I80_FREQ, b->freq_hz);
    }

    const nc_devices_cfg_t *dv = nc_cfg_devices();
    if (dv != NULL && dv->present) {
        const nc_led_cfg_t *led = &dv->led;
        n = put_b(n, K_LED_ENABLE, led->enable);
        n = put_u(n, K_LED_BACKEND, led->backend);
        if (dv->led_decl) {
            n = put_i(n, K_LED_BUS, led->bus);
            n = put_u(n, K_LED_LANES, led->lanes);
            n = put_u(n, K_LED_PX, led->px);
            if (led->fmt[0] != '\0') {
                n = put_s(n, K_LED_FMT, led->fmt);
            }
            n = put_u(n, K_LED_CLK, led->clk_hz);
            n = put_u(n, K_LED_BIT_CELLS, led->bit_cells);
            n = put_u(n, K_LED_T0H, led->t0h);
            n = put_u(n, K_LED_T1H, led->t1h);
        }
    }
    return n;
}

static const char *miss_reason(esp_err_t e)
{
    switch (e) {
    case ESP_OK:                return "config 改咗（src_hash 唔同）";
    case ESP_ERR_NOT_FOUND:     return "未有檔";
    case ESP_ERR_INVALID_STATE: return "magic／ver 唔夾";
    case ESP_ERR_INVALID_SIZE:  return "截斷／長度唔夾";
    case ESP_FAIL:              return "CRC 壞";
    default:                    return "讀取失敗";
    }
}

esp_err_t nc_state_boot_sync(nc_state_boot_result_t *out)
{
    nc_state_boot_result_t r;
    memset(&r, 0, sizeof(r));

    uint32_t src_hash = 0;
    const esp_err_t he = nc_state_file_hash_files(k_src_paths,
                            sizeof(k_src_paths) / sizeof(k_src_paths[0]), &src_hash);
    if (he != ESP_OK) {
        ESP_LOGW(TAG, "NCSTATE：算 src_hash 失敗（%s）", esp_err_to_name(he));
        if (out != NULL) {
            *out = r;
        }
        return he;
    }
    r.src_hash = src_hash;
    r.n_records = (uint32_t)build_records();

    nc_state_info_t info;
    const esp_err_t pe = nc_state_file_peek(NC_STATE_PATH, &info);
    if (pe == ESP_OK && info.src_hash == src_hash) {
        r.hit = true;
        r.bytes = nc_state_file_size(NC_STATE_PATH);
        ESP_LOGI(TAG, "NCSTATE hit：src=0x%08x、%u 筆、%u B",
                 (unsigned)src_hash, (unsigned)info.n_records, (unsigned)r.bytes);
    } else {
        r.why = miss_reason(pe);
        const nc_state_meta_t meta = {
            .ver = NC_STATE_BLOB_VER,
            .flags = 0,
            .src_hash = src_hash,
        };
        const esp_err_t se = nc_state_file_save(NC_STATE_PATH, &meta, s_recs, r.n_records);
        if (se == ESP_OK) {
            r.rebuilt = true;
            r.bytes = nc_state_file_size(NC_STATE_PATH);
            ESP_LOGW(TAG, "NCSTATE miss（%s）→ 重建：src=0x%08x、%u 筆、%u B（舊檔 → .bak）",
                     r.why, (unsigned)src_hash, (unsigned)r.n_records, (unsigned)r.bytes);
        } else {
            ESP_LOGE(TAG, "NCSTATE miss（%s）→ 重建失敗（%s）；L1 JSON／L0 預設仍然有效",
                     r.why, esp_err_to_name(se));
        }
    }
    if (out != NULL) {
        *out = r;
    }
    return ESP_OK;
}
