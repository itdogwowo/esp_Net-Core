// nc_plugin.c — 插件工具的外殼：backend 註冊、暫停閘、四個動詞的轉發。
//
// 這裡刻意冇政策：冇 scan、冇 arena、冇 live 指標、冇預算、冇 task attach。
// 那些屬於客戶端（見 gap-review §6.4.1）；工具只答「開得成唔成、跑得成唔成」。
#include "nc_plugin.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "plugin";

static const nc_plugin_backend_t *s_be;
static bool s_init_ok;
static bool (*s_pause_fn)(void);
static void *s_arena;
static size_t s_arena_bytes;

void nc_plugin_set_arena(void *arena, size_t bytes)
{
    s_arena = arena;
    s_arena_bytes = bytes;
}

esp_err_t nc_plugin_set_backend(const nc_plugin_backend_t *be)
{
    s_be = NULL;
    s_init_ok = false;

    if (be == NULL) {
        return ESP_OK;                  // 明確清除（host 測試／換引擎）
    }
    if (be->open == NULL || be->step == NULL) {
        ESP_LOGE(TAG, "backend %s 唔完整（open／step 必填）", be->name ? be->name : "?");
        return ESP_ERR_INVALID_ARG;
    }
    s_be = be;
    if (be->init != NULL) {
        const esp_err_t err = be->init(s_arena, s_arena_bytes);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "backend %s init 失敗：%s", be->name ? be->name : "?",
                     esp_err_to_name(err));
            s_be = NULL;
            return err;
        }
    }
    s_init_ok = true;
    ESP_LOGI(TAG, "backend=%s%s", be->name ? be->name : "?",
             (s_arena != NULL) ? "（已 init）" : "（冇 arena）");
    return ESP_OK;
}

bool nc_plugin_ready(void)
{
    return s_init_ok && s_be != NULL;
}

const char *nc_plugin_backend_name(void)
{
    return (s_be != NULL && s_be->name != NULL) ? s_be->name : "none";
}

esp_err_t nc_plugin_open(nc_plugin_t *out, const nc_plugin_src_t *src)
{
    if (out == NULL || src == NULL || src->payload == NULL || src->size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (!nc_plugin_ready()) {
        return ESP_ERR_INVALID_STATE;   // 冇引擎：載入得但跑唔到（客戶端自己決定）
    }
    return s_be->open(out, src);
}

esp_err_t nc_plugin_step(nc_plugin_t p, uint32_t frame_id)
{
    if (!nc_plugin_ready() || p == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return s_be->step(p, frame_id);
}

esp_err_t nc_plugin_frame(nc_plugin_t p, const uint8_t **fb, uint32_t *bytes)
{
    if (fb == NULL || bytes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!nc_plugin_ready() || p == NULL || s_be->frame == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return s_be->frame(p, fb, bytes);
}

void nc_plugin_close(nc_plugin_t p)
{
    if (p != NULL && s_be != NULL && s_be->close != NULL) {
        s_be->close(p);
    }
}

void nc_plugin_deinit(void)
{
    if (s_be != NULL && s_be->deinit != NULL) {
        s_be->deinit();
    }
    s_init_ok = false;
    s_be = NULL;
}

void nc_plugin_set_pause_check(bool (*fn)(void))
{
    s_pause_fn = fn;
}

bool nc_plugin_paused(void)
{
    return (s_pause_fn != NULL) && s_pause_fn();
}

// ABI v0 descriptor：guest 開機讀它才知道 px／fb 在哪。
// 純函式（唔碰 WAMR），所以 host 測試可以直接驗佈局。
bool nc_plugin_desc_build(uint16_t px, uint32_t mem_bytes, nc_mod_desc_t *out)
{
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    // `.nce` 交的是「一串 0–255」，每值 1 B；格式（r/g/b/w/rgb/rgbw）由配對層決定。
    const uint32_t fb_bytes = (uint32_t)px;
    const uint32_t need = NC_PLUGIN_FB_OFF + fb_bytes;
    if (mem_bytes != 0 && need > mem_bytes) {
        return false;                  // fb 放唔落：唔好開一個一定會 trap 的插件
    }
    out->px_count = px;
    out->width = 0;
    out->height = 0;
    out->fb_off = NC_PLUGIN_FB_OFF;
    out->fb_bytes = fb_bytes;
    out->sc_off = NC_PLUGIN_SCRATCH_OFF;
    out->sc_bytes = NC_PLUGIN_SCRATCH_BYTES;
    return true;
}

// 通用 region 綁定：純算術，唔碰 backend。方向／名字只係描述，唔影響排放。
bool nc_plugin_regions_bind(const nc_plugin_region_t *r, int n,
                            uint32_t first_off, uint32_t limit, uint32_t *offsets)
{
    if (r == NULL || offsets == NULL || n < 0 || n > NC_PLUGIN_REGION_MAX) {
        return false;
    }
    uint32_t off = first_off;
    for (int i = 0; i < n; i++) {
        if (r[i].dir != NC_PLUGIN_REGION_IN && r[i].dir != NC_PLUGIN_REGION_OUT) {
            return false;
        }
        uint32_t a = r[i].align ? r[i].align : 16u;
        if (a & (a - 1u)) {                       // 只收 2 的次方
            return false;
        }
        off = (off + a - 1u) & ~(a - 1u);
        if (r[i].bytes > limit || off > limit - r[i].bytes) {
            return false;                         // 放唔落：唔縮、唔覆蓋
        }
        offsets[i] = off;
        off += r[i].bytes;
    }
    return true;
}
