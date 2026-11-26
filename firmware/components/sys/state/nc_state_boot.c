// 開機 sync state.ncst：由 config 派生＋對拍（P3 第一步）。
//
// 流程（定案：開機對哈希；一樣＝直接載入；唔一樣＝重新生成）：
//   1. 算 config JSON 的 src_hash（hardware／driver／system／tasks／resources）
//   2. peek state.ncst：ver／magic／CRC／src_hash 全中 → 載入 RAM store
//   3. 同「由已 parse 的 cfg 造出的 records」**對拍**：
//        - 全中 → hit（log: 一致 n/n）
//        - 有唔同 → 當 miss（log: 唔一致 first key）→ 重寫（self-heal，防舊 cache 靜默生效）
//   4. 檔案唔存在／壞／版本唔夾 → 由 cfg 造 records → atomic 寫檔（舊檔 → .bak）
//
// 保護層：L0 出廠預設（firmware const）／L1 JSON（權威）／L2 state.ncst（快取）／
//         L2b .bak（last-known-good）。任何一層壞都唔會令設定「靜靜變零」。
#include "nc_state_boot.h"

#include <string.h>

#include "esp_log.h"
#include "nc_cfg.h"
#include "nc_state_blob.h"
#include "nc_state_file.h"
#include "nc_state_keys.h"
#include "nc_state_schema.h"
#include "nc_state_store.h"

static const char *TAG = "state";

static nc_state_rec_t s_recs[32];
static nc_state_store_t s_store;          // 常駐 RAM：blob 載入後的值（~576 B internal）
static nc_state_boot_result_t s_result;
static bool s_result_valid;

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
        n = put_u(n, NC_KEY_SYS_FRAME_MS, sys->frame_interval_ms);
        n = put_u(n, NC_KEY_SYS_NUM_PIXELS, sys->num_pixels);
        n = put_u(n, NC_KEY_SYS_LOG_DEBUG, sys->log_debug_mode);
    }

    const nc_hardware_cfg_t *hw = nc_cfg_hardware();
    if (hw != NULL && hw->n_i80 > 0) {
        const nc_i80_cfg_t *b = &hw->i80[0];
        n = put_u(n, NC_KEY_I80_COUNT, b->n_used);
        n = put_i(n, NC_KEY_I80_WR, b->wr);
        n = put_i(n, NC_KEY_I80_DC, b->dc);
        n = put_i(n, NC_KEY_I80_CS, b->cs);
        n = put_u(n, NC_KEY_I80_FREQ, b->freq_hz);
    }
    if (hw != NULL && hw->n_spi > 0) {
        const nc_spi_cfg_t *s = &hw->spi[0];
        n = put_b(n, NC_KEY_SPI_ENABLE, s->enable);
        n = put_u(n, NC_KEY_SPI_BAUD, s->baud);
        n = put_u(n, NC_KEY_SPI_PHASE, s->phase);
        n = put_u(n, NC_KEY_SPI_POL, s->polarity);
        n = put_i(n, NC_KEY_SPI_SCK, s->sck);
        n = put_i(n, NC_KEY_SPI_MOSI, s->mosi);
        n = put_i(n, NC_KEY_SPI_MISO, s->miso);
    }

    const nc_devices_cfg_t *dv = nc_cfg_devices();
    if (dv != NULL && dv->present) {
        const nc_led_cfg_t *led = &dv->led;
        n = put_b(n, NC_KEY_LED_ENABLE, led->enable);
        n = put_u(n, NC_KEY_LED_BACKEND, led->backend);
        if (dv->led_decl) {
            n = put_i(n, NC_KEY_LED_BUS, led->bus);
            n = put_u(n, NC_KEY_LED_LANES, led->lanes);
            n = put_u(n, NC_KEY_LED_PX, led->px);
            if (led->fmt[0] != '\0') {
                n = put_s(n, NC_KEY_LED_FMT, led->fmt);
            }
            n = put_u(n, NC_KEY_LED_CLK, led->clk_hz);
            n = put_u(n, NC_KEY_LED_BIT_CELLS, led->bit_cells);
            n = put_u(n, NC_KEY_LED_T0H, led->t0h);
            n = put_u(n, NC_KEY_LED_T1H, led->t1h);
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

static bool save_records(const nc_state_meta_t *meta, size_t nrec)
{
    const esp_err_t se = nc_state_file_save(NC_STATE_PATH, meta, s_recs, nrec);
    if (se != ESP_OK) {
        ESP_LOGE(TAG, "NCSTATE 寫檔失敗（%s）；L1 JSON／L0 預設仍然有效",
                 esp_err_to_name(se));
        return false;
    }
    (void)nc_state_store_from_recs(&s_store, s_recs, nrec);
    return true;
}

esp_err_t nc_state_boot_sync(nc_state_boot_result_t *out)
{
    memset(&s_result, 0, sizeof(s_result));
    memset(&s_store, 0, sizeof(s_store));

    // src_hash 由 nc_cfg_boot() 讀檔時順便計（唔再重讀 13 KB；曾量到 24 ms）。
    // 次序 = cfg boot 開檔次序：system、hardware、tasks、resources、driver。
    uint32_t src_hash = nc_cfg_raw_hash();
    if (nc_cfg_raw_hash_files() == 0) {
        ESP_LOGW(TAG, "NCSTATE：cfg 未 boot（冇 hash）→ 跳過 state sync");
        if (out != NULL) {
            *out = s_result;
        }
        return ESP_ERR_INVALID_STATE;
    }
    s_result.src_hash = src_hash;
    s_result.n_records = (uint32_t)build_records();

    // schema 係唯一名單：由 cfg 造出的 record 一定要同 schema 對得上（型別／範圍）。
    // 對唔上唔會靜靜收貨：計數＋逐筆 log（最常見 = 加咗 key 但 schema 未加）。
    for (uint32_t i = 0; i < s_result.n_records; i++) {
        const uint8_t chk = nc_state_schema_check(&s_recs[i]);
        if (chk != NC_STATE_CHK_OK) {
            s_result.n_schema_bad++;
            ESP_LOGW(TAG, "state record 0x%04x 同 schema 唔一致（%s）",
                     (unsigned)s_recs[i].key, nc_state_schema_check_name(chk));
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
    uint16_t first = 0;
    if (pe == ESP_OK) {
        nc_state_info_t li;
        memset(&s_store, 0, sizeof(s_store));
        const esp_err_t le = nc_state_file_load(NC_STATE_PATH, nc_state_store_cb,
                                                &s_store, &li);
        if (le == ESP_OK) {
            cache_ok = true;
            diff = nc_state_store_cmp(&s_store, s_recs, s_result.n_records, &first);
            s_result.n_skipped = li.n_skipped;
        }
    }

    bool matched = false;
    if (hash_match && cache_ok && diff == 0) {
        matched = true;
        s_result.cmp_ok = true;
        s_result.hit = true;
        s_result.bytes = nc_state_file_size(NC_STATE_PATH);
        ESP_LOGI(TAG, "NCSTATE hit：src=0x%08x、%u 筆、%u B（同 cfg 完全一致）",
                 (unsigned)src_hash, (unsigned)s_result.n_records, (unsigned)s_result.bytes);
    } else if (cache_ok && diff == 0) {
        // hash 變咗（例如改 comment／tasks 等唔關 state 嘅內容）但 state 值冇變：
        // 保留舊檔、唔寫盤。status: hit=0, cmp_ok=1, rebuilt=0（值一致，版本戳過時）。
        matched = true;      // 「值」已經正確，唔使重建
        s_result.cmp_ok = true;
        s_result.why = "config hash 改咗但 state 值一樣";
        s_result.bytes = nc_state_file_size(NC_STATE_PATH);
        ESP_LOGW(TAG, "NCSTATE miss（%s）→ 值一樣，唔重寫（省 WL 寫盤）",
                 s_result.why);
    } else {
        s_result.n_mismatch = diff;
        s_result.first_diff = first;
        s_result.why = (pe != ESP_OK) ? miss_reason(pe) : "cache 同 cfg 唔一致";
        if (save_records(&meta, s_result.n_records)) {
            s_result.rebuilt = true;
            s_result.bytes = nc_state_file_size(NC_STATE_PATH);
            ESP_LOGW(TAG, "NCSTATE miss（%s）→ 重建：src=0x%08x、%u 筆、%u B（舊檔 → .bak）",
                     s_result.why, (unsigned)src_hash, (unsigned)s_result.n_records,
                     (unsigned)s_result.bytes);
        }
    }
    (void)matched;

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
