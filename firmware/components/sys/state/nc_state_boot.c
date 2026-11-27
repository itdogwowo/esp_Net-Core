// 開機 sync state.ncst：由 config 派生＋對拍（P3 第一步）。
//
// 流程（定案：開機對哈希；一樣＝直接載入；唔一樣＝重新生成）：
//   1. 由 cfg 造 records，算**值**的 src_hash（nc_state_blob_rec_crc）
//   2. peek state.ncst：ver／magic／CRC／src_hash 全中 → 載入 RAM store
//   3. 同「由已 parse 的 cfg 造出的 records」**對拍**：
//        - 全中 → hit（log: 一致 n/n）
//        - 有唔同 → 當 miss（log: 唔一致 first key）→ 重寫（self-heal，防舊 cache 靜默生效）
//   4. 檔案唔存在／壞／版本唔夾 → 由 cfg 造 records → atomic 寫檔（舊檔 → .bak）
//
// 保護層：L0 出廠預設（firmware const）／L1 JSON（權威）／L2 state.ncst（快取）／
//         L2b .bak（last-known-good）。任何一層壞都唔會令設定「靜靜變零」。
#include "nc_state_boot.h"

#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nc_cfg.h"
#include "nc_state_blob.h"
#include "nc_state_file.h"
#include "nc_state_keys.h"
#include "nc_state_schema.h"
#include "nc_state_store.h"

static const char *TAG = "state";

// 開機 sync 的 records 暫存（32 筆 × 24 B = 768 B）。
// **只在 B02 用一次**，之後完全唔會再讀 → 冇理由長佔 SRAM：
//   有 PSRAM 的板 → 開機一次性由 PSRAM 拿（失敗就明示跳過 state sync，cfg 仍然權威）
//   冇 PSRAM 的板（C3 等）→ compile-time 用 internal 靜態陣列
#define NC_STATE_RECS_MAX 32
#if CONFIG_SPIRAM
static nc_state_rec_t *s_recs;          // 指向 PSRAM（4 B pointer）
static nc_state_rec_t *recs_buf(void)
{
    if (s_recs == NULL) {
        s_recs = (nc_state_rec_t *)heap_caps_malloc(
            sizeof(nc_state_rec_t) * NC_STATE_RECS_MAX, MALLOC_CAP_SPIRAM);
        if (s_recs == NULL) {
            ESP_LOGW(TAG, "PSRAM 唔夠 → 跳過 state sync（cfg／JSON 仍然權威，唔會靜默）");
        }
    }
    return s_recs;
}
#else
static nc_state_rec_t s_recs[NC_STATE_RECS_MAX];
static nc_state_rec_t *recs_buf(void) { return s_recs; }
#endif
static nc_state_store_t s_store;          // 常駐 RAM：blob 載入後的值（~576 B internal）
static nc_state_boot_result_t s_result;
static bool s_result_valid;

static size_t put_u(nc_state_rec_t *recs, size_t n, uint32_t key, uint32_t v)
{
    recs[n] = (nc_state_rec_t){ .key = key, .type = NC_STATE_T_UINT, .u = v };
    return n + 1;
}

static size_t put_i(nc_state_rec_t *recs, size_t n, uint32_t key, int32_t v)
{
    recs[n] = (nc_state_rec_t){ .key = key, .type = NC_STATE_T_SINT, .i = v };
    return n + 1;
}

static size_t put_b(nc_state_rec_t *recs, size_t n, uint32_t key, bool v)
{
    recs[n] = (nc_state_rec_t){ .key = key, .type = NC_STATE_T_BOOL, .u = v ? 1u : 0u };
    return n + 1;
}

// 由已 parse 的 cfg 造 record（sparse：只寫真的值；default 留給 schema／L0）
static size_t build_records(nc_state_rec_t *recs)
{
    size_t n = 0;

    const nc_system_cfg_t *sys = nc_cfg_system();
    if (sys != NULL) {
        n = put_u(recs, n, NC_KEY_SYS_FRAME_MS, sys->frame_interval_ms);
        n = put_u(recs, n, NC_KEY_SYS_NUM_PIXELS, sys->num_pixels);
        n = put_u(recs, n, NC_KEY_SYS_LOG_DEBUG, sys->log_debug_mode);
    }

    if (sys != NULL && sys->cid[0] != '\0') {
        // cID：JSON 寫 4 個 hex 字（"F5F9"）；0 或空 = 由 MAC 推導（唔寫 record）。
        char *end = NULL;
        const unsigned long v = strtoul(sys->cid, &end, 16);
        if (end != NULL && *end == '\0' && v > 0 && v <= 0xFFFFul) {
            n = put_u(recs, n, NC_KEY_SYS_CID, (uint32_t)v);
        } else {
            ESP_LOGW(TAG, "system.json cID '%s' 唔係合法 hex → 跳過", sys->cid);
        }
    }

    const nc_hardware_cfg_t *hw = nc_cfg_hardware();
    if (hw != NULL && hw->n_i80 > 0) {
        const nc_i80_cfg_t *b = &hw->i80[0];
        n = put_u(recs, n, NC_KEY_I80_COUNT, b->n_used);
        n = put_i(recs, n, NC_KEY_I80_WR, b->wr);
        n = put_i(recs, n, NC_KEY_I80_DC, b->dc);
        n = put_i(recs, n, NC_KEY_I80_CS, b->cs);
        n = put_u(recs, n, NC_KEY_I80_FREQ, b->freq_hz);
    }
    if (hw != NULL && hw->n_spi > 0) {
        const nc_spi_cfg_t *s = &hw->spi[0];
        n = put_b(recs, n, NC_KEY_SPI_ENABLE, s->enable);
        n = put_u(recs, n, NC_KEY_SPI_BAUD, s->baud);
        n = put_u(recs, n, NC_KEY_SPI_PHASE, s->phase);
        n = put_u(recs, n, NC_KEY_SPI_POL, s->polarity);
        n = put_i(recs, n, NC_KEY_SPI_SCK, s->sck);
        n = put_i(recs, n, NC_KEY_SPI_MOSI, s->mosi);
        n = put_i(recs, n, NC_KEY_SPI_MISO, s->miso);
    }

    const nc_devices_cfg_t *dv = nc_cfg_devices();
    if (dv != NULL && dv->present) {
        const nc_led_cfg_t *led = &dv->led;
        n = put_b(recs, n, NC_KEY_LED_ENABLE, led->enable);
        n = put_u(recs, n, NC_KEY_LED_BACKEND, led->backend);
        if (dv->led_decl) {
            n = put_i(recs, n, NC_KEY_LED_BUS, led->bus);
            n = put_u(recs, n, NC_KEY_LED_LANES, led->lanes);
            n = put_u(recs, n, NC_KEY_LED_PX, led->px);
            if (led->fmt[0] != '\0') {
                // JSON 寫人讀名（"ws2812"），state 只存 index（enum）：
                // 名字留喺 schema 字典（flash），唔入 store／blob。
                const int fid = nc_state_enum_id(nc_state_key(NC_KEY_LED_FMT), led->fmt);
                if (fid >= 0) {
                    n = put_u(recs, n, NC_KEY_LED_FMT, (uint32_t)fid);
                } else {
                    ESP_LOGW(TAG, "LED format '%s' 唔在 schema enum 內 → 跳過呢個 key",
                             led->fmt);
                }
            }
            n = put_u(recs, n, NC_KEY_LED_CLK, led->clk_hz);
            n = put_u(recs, n, NC_KEY_LED_BIT_CELLS, led->bit_cells);
            n = put_u(recs, n, NC_KEY_LED_T0H, led->t0h);
            n = put_u(recs, n, NC_KEY_LED_T1H, led->t1h);
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

static bool save_records(const nc_state_meta_t *meta, const nc_state_rec_t *recs, size_t nrec)
{
    const esp_err_t se = nc_state_file_save(NC_STATE_PATH, meta, recs, nrec);
    if (se != ESP_OK) {
        ESP_LOGE(TAG, "NCSTATE 寫檔失敗（%s）；L1 JSON／L0 預設仍然有效",
                 esp_err_to_name(se));
        return false;
    }
    (void)nc_state_store_from_recs(&s_store, recs, nrec);
    return true;
}

esp_err_t nc_state_boot_sync(nc_state_boot_result_t *out)
{
    memset(&s_result, 0, sizeof(s_result));
    memset(&s_store, 0, sizeof(s_store));

    if (nc_cfg_raw_hash_files() == 0) {
        ESP_LOGW(TAG, "NCSTATE：cfg 未 boot（冇 hash）→ 跳過 state sync");
        if (out != NULL) {
            *out = s_result;
        }
        return ESP_ERR_INVALID_STATE;
    }
    nc_state_rec_t *const recs = recs_buf();
    if (recs == NULL) {
        if (out != NULL) {
            *out = s_result;
        }
        return ESP_ERR_NO_MEM;      // 明示失敗；state 係快取，cfg／JSON 仍然權威
    }
    s_result.n_records = (uint32_t)build_records(recs);

    // src_hash = **值**的 CRC（2026-11-27 由「JSON 檔案 bytes 的 hash」改過來）：
    // 改一句 _comment／排版／唔關事嘅檔，唔應該令所有回寫值失效。
    // 真係改咗值 → hash 變 → 重建（呢個正是 cache 失效的條件）。
    const uint32_t src_hash = nc_state_blob_rec_crc(recs, s_result.n_records);
    if (src_hash == 0) {
        ESP_LOGW(TAG, "NCSTATE：由 cfg 造出的 record 值唔合法 → 跳過 state sync");
        if (out != NULL) {
            *out = s_result;
        }
        return ESP_ERR_INVALID_STATE;
    }
    s_result.src_hash = src_hash;

    // schema 係唯一名單：由 cfg 造出的 record 一定要同 schema 對得上（型別／範圍）。
    // 對唔上唔會靜靜收貨：計數＋逐筆 log（最常見 = 加咗 key 但 schema 未加）。
    for (uint32_t i = 0; i < s_result.n_records; i++) {
        const uint8_t chk = nc_state_schema_check(&recs[i]);
        if (chk != NC_STATE_CHK_OK) {
            s_result.n_schema_bad++;
            ESP_LOGW(TAG, "state record 0x%08x 同 schema 唔一致（%s）",
                     (unsigned)recs[i].key, nc_state_schema_check_name(chk));
        }
    }

    const nc_state_meta_t meta = {
        .ver = NC_STATE_BLOB_VER,
        .flags = 0,
        .src_hash = src_hash,
    };

    nc_state_info_t info;
    const esp_err_t pe = nc_state_file_peek(NC_STATE_PATH, &info);
    const bool hash_match = (pe == ESP_OK && info.src_hash == src_hash);

    // 無論 hash 是否對得上，都試載入舊 blob 對拍：
    //   - hash 對 + 值全中 → 真 hit；
    //   - hash 唔對但值一樣 → **唔重寫**（WL FAT 寫 124 B 實測要 ~762 ms，避免無謂寫盤）；
    //   - 值有唔同／舊檔壞 → 先寫（唯一必要的一次性成本）。
    bool cache_ok = false;
    uint32_t diff = 0xFFFFFFFFu;
    uint32_t first = 0;
    if (pe == ESP_OK) {
        nc_state_info_t li;
        memset(&s_store, 0, sizeof(s_store));
        const esp_err_t le = nc_state_file_load(NC_STATE_PATH, nc_state_store_cb,
                                                &s_store, &li);
        if (le == ESP_OK) {
            cache_ok = true;
            diff = nc_state_store_cmp(&s_store, recs, s_result.n_records, &first);
            s_result.n_skipped = li.n_skipped;
        }
    }

    // 判定只看「值」：src_hash 係診斷用的版本戳，唔做閘門。
    // 為什麼：hash 一改語義（例如 2026-11-27 由 bytes-hash 改成值-hash）就會永遠
    // 對唔上，但值其實一樣 —— 若以 hash 做閘門，hit 會永遠 0，冇人信個指標。
    if (cache_ok && diff == 0) {
        s_result.cmp_ok = true;
        s_result.hit = true;
        s_result.bytes = nc_state_file_size(NC_STATE_PATH);
        if (!hash_match) {
            s_result.why = "值一樣、src_hash 過時（唔重寫，省 WL 寫盤）";
            ESP_LOGW(TAG, "NCSTATE hit（%s）：src=0x%08x／file=0x%08x、%u 筆、%u B",
                     s_result.why, (unsigned)src_hash, (unsigned)info.src_hash,
                     (unsigned)s_result.n_records, (unsigned)s_result.bytes);
        } else {
            ESP_LOGI(TAG, "NCSTATE hit：src=0x%08x、%u 筆、%u B（同 cfg 完全一致）",
                     (unsigned)src_hash, (unsigned)s_result.n_records, (unsigned)s_result.bytes);
        }
    } else {
        s_result.n_mismatch = diff;
        s_result.first_diff = first;
        s_result.why = (pe != ESP_OK) ? miss_reason(pe) : "cache 同 cfg 唔一致";
        if (save_records(&meta, recs, s_result.n_records)) {
            s_result.rebuilt = true;
            s_result.bytes = nc_state_file_size(NC_STATE_PATH);
            ESP_LOGW(TAG, "NCSTATE miss（%s）→ 重建：src=0x%08x、%u 筆、%u B（舊檔 → .bak）",
                     s_result.why, (unsigned)src_hash, (unsigned)s_result.n_records,
                     (unsigned)s_result.bytes);
        }
    }

    s_result_valid = true;
    if (out != NULL) {
        *out = s_result;
    }
    return ESP_OK;
}

const nc_state_boot_result_t *nc_state_boot_last(void)
{
    return s_result_valid ? &s_result : NULL;
}

const nc_state_store_t *nc_state_boot_store(void)
{
    return (s_store.n > 0) ? &s_store : NULL;
}

nc_state_store_t *nc_state_boot_store_mut(void)
{
    // 用 s_result_valid 而唔係 n：sync 跑過但 0 筆（全部用預設）都算「準備好」，
    // 之後 nc_state_set_* 仍然可以寫入。
    return s_result_valid ? &s_store : NULL;
}
