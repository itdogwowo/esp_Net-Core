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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nc_cfg.h"
#include "nc_state_api.h"
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
static uint32_t s_src_hash;      // 值集 hash（commit 沿用）

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
    if (hw != NULL && hw->n_i80 > 0 && nc_state_unit_active(NC_UNIT_I80)) {
        const nc_i80_cfg_t *b = &hw->i80[0];
        n = put_u(recs, n, NC_KEY_I80_COUNT, b->n_used);
        n = put_i(recs, n, NC_KEY_I80_WR, b->wr);
        n = put_i(recs, n, NC_KEY_I80_DC, b->dc);
        n = put_i(recs, n, NC_KEY_I80_CS, b->cs);
        n = put_u(recs, n, NC_KEY_I80_FREQ, b->freq_hz);
    }
    if (hw != NULL && hw->n_spi > 0 && nc_state_unit_active(NC_UNIT_SPI)) {
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
    if (dv != NULL && dv->present && nc_state_unit_active(NC_UNIT_LED)) {
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
    // S4：邊啲 unit 啟動（開機由 cfg 決定；未啟動 = 唔造 record、set 回 not_active）
    {
        uint16_t mask = (uint16_t)((1u << NC_UNIT_SYS) | (1u << NC_UNIT_NET));
        const nc_hardware_cfg_t *hw0 = nc_cfg_hardware();
        if (hw0 != NULL) {
            if (hw0->n_i80 > 0 && hw0->i80[0].enable) mask |= (uint16_t)(1u << NC_UNIT_I80);
            if (hw0->n_spi > 0 && hw0->spi[0].enable) mask |= (uint16_t)(1u << NC_UNIT_SPI);
        }
        const nc_devices_cfg_t *dv0 = nc_cfg_devices();
        if (dv0 != NULL && dv0->present && dv0->led_decl && dv0->led.enable) {
            mask |= (uint16_t)(1u << NC_UNIT_LED);
        }
        nc_state_active_units_set(mask);
        s_result.n_inactive = 0;
        for (uint32_t i = 0; i < nc_state_key_count(); i++) {
            const nc_state_keydef_t *d = nc_state_key_at(i);
            if (d != NULL && d->cls == NC_STATE_CLS_VALUE && !nc_state_unit_active(d->unit)) {
                s_result.n_inactive++;
            }
        }
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
    s_src_hash = src_hash;

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

    // 定案（S3，2026-11-27）：
    //   hash 對得上 ＝ 「config 冇改過」→ **state（override）贏**：保留 blob 裡的值，
    //     只 prune 走停用 unit 的舊記錄。唔可以用「值同 cfg 唔同」做重建條件，
    //     否則回寫值一 reboot 就冇（實測 frame_ms=25 重啟變回 20）。
    //   hash 唔對 ＝ config 改過 → **JSON 贏**：由 cfg 重建（回寫值清掉，可預期）。
    //   冇檔／壞檔 → 由 cfg 重建。
    if (cache_ok && hash_match) {
        s_result.hit = true;
        s_result.cmp_ok = (diff == 0);
        s_result.n_mismatch = (diff == 0) ? 0 : diff;
        s_result.first_diff = first;
        s_result.bytes = nc_state_file_size(NC_STATE_PATH);
        const uint16_t dropped = nc_state_store_prune(&s_store, recs, s_result.n_records);
        if (diff == 0) {
            ESP_LOGI(TAG, "NCSTATE hit：src=0x%08x、%u 筆、%u B（同 cfg 一致）",
                     (unsigned)src_hash, (unsigned)s_result.n_records, (unsigned)s_result.bytes);
        } else {
            ESP_LOGW(TAG, "NCSTATE hit（state 贏）：%u 筆 override（first=0x%08x）、"
                     "prune %u 筆舊記錄、%u B",
                     (unsigned)diff, (unsigned)first, (unsigned)dropped,
                     (unsigned)s_result.bytes);
        }
    } else {
        s_result.n_mismatch = diff;
        s_result.first_diff = first;
        s_result.why = (pe != ESP_OK) ? miss_reason(pe)
                       : "config 改過（src_hash 唔同）";
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

uint32_t nc_state_boot_src_hash(void)
{
    return s_src_hash;
}

// ── S2：store → cfg（B02 之後、B05 之前）──────────────────────────
static bool get_u(uint32_t key, uint32_t *v)
{
    nc_state_rec_t r;
    if (!nc_state_store_get(nc_state_boot_store(), key, &r) || r.type != NC_STATE_T_UINT) {
        return false;
    }
    *v = r.u;
    return true;
}

static bool get_i(uint32_t key, int32_t *v)
{
    nc_state_rec_t r;
    if (!nc_state_store_get(nc_state_boot_store(), key, &r) || r.type != NC_STATE_T_SINT) {
        return false;
    }
    *v = r.i;
    return true;
}

static bool get_b(uint32_t key, bool *v)
{
    nc_state_rec_t r;
    if (!nc_state_store_get(nc_state_boot_store(), key, &r) || r.type != NC_STATE_T_BOOL) {
        return false;
    }
    *v = r.u != 0;
    return true;
}

uint32_t nc_state_apply_to_cfg(void)
{
    uint32_t n = 0;
    uint32_t u = 0;
    int32_t iv = 0;
    bool bv = false;

    nc_system_cfg_t *sys = nc_cfg_system_mut();
    if (sys != NULL) {
        if (get_u(NC_KEY_SYS_FRAME_MS, &u)) { sys->frame_interval_ms = (uint16_t)u; n++; }
        if (get_u(NC_KEY_SYS_NUM_PIXELS, &u)) { sys->num_pixels = (uint16_t)u; n++; }
        if (get_u(NC_KEY_SYS_LOG_DEBUG, &u)) { sys->log_debug_mode = (uint8_t)u; n++; }
        if (get_u(NC_KEY_SYS_CID, &u)) {
            (void)snprintf(sys->cid, sizeof(sys->cid), "%04X", (unsigned)u);
            n++;
        }
    }

    nc_hardware_cfg_t *hw = nc_cfg_hardware_mut();
    if (hw != NULL && hw->n_i80 > 0 && nc_state_unit_active(NC_UNIT_I80)) {
        nc_i80_cfg_t *b = &hw->i80[0];
        if (get_u(NC_KEY_I80_COUNT, &u)) { b->n_used = (uint8_t)u; n++; }
        if (get_i(NC_KEY_I80_WR, &iv)) { b->wr = (int8_t)iv; n++; }
        if (get_i(NC_KEY_I80_DC, &iv)) { b->dc = (int8_t)iv; n++; }
        if (get_i(NC_KEY_I80_CS, &iv)) { b->cs = (int8_t)iv; n++; }
        if (get_u(NC_KEY_I80_FREQ, &u)) { b->freq_hz = u; n++; }
    }
    if (hw != NULL && hw->n_spi > 0 && nc_state_unit_active(NC_UNIT_SPI)) {
        nc_spi_cfg_t *sp = &hw->spi[0];
        if (get_b(NC_KEY_SPI_ENABLE, &bv)) { sp->enable = bv; n++; }
        if (get_u(NC_KEY_SPI_BAUD, &u)) { sp->baud = u; n++; }
        if (get_u(NC_KEY_SPI_PHASE, &u)) { sp->phase = (uint8_t)u; n++; }
        if (get_u(NC_KEY_SPI_POL, &u)) { sp->polarity = (uint8_t)u; n++; }
        if (get_i(NC_KEY_SPI_SCK, &iv)) { sp->sck = (int8_t)iv; n++; }
        if (get_i(NC_KEY_SPI_MOSI, &iv)) { sp->mosi = (int8_t)iv; n++; }
        if (get_i(NC_KEY_SPI_MISO, &iv)) { sp->miso = (int8_t)iv; n++; }
    }
    nc_devices_cfg_t *dv = nc_cfg_devices_mut();
    if (dv != NULL && dv->present && dv->led_decl && nc_state_unit_active(NC_UNIT_LED)) {
        nc_led_cfg_t *led = &dv->led;
        if (get_b(NC_KEY_LED_ENABLE, &bv)) { led->enable = bv; n++; }
        if (get_u(NC_KEY_LED_BACKEND, &u)) { led->backend = (uint8_t)u; n++; }
        if (get_i(NC_KEY_LED_BUS, &iv)) { led->bus = (int8_t)iv; n++; }
        if (get_u(NC_KEY_LED_LANES, &u)) { led->lanes = (uint8_t)u; n++; }
        if (get_u(NC_KEY_LED_PX, &u)) { led->px = (uint16_t)u; n++; }
        if (get_u(NC_KEY_LED_FMT, &u)) {
            const char *nm = nc_state_enum_name(nc_state_key(NC_KEY_LED_FMT), u);
            if (nm != NULL) {
                (void)snprintf(led->fmt, sizeof(led->fmt), "%s", nm);
                n++;
            }
        }
        if (get_u(NC_KEY_LED_CLK, &u)) { led->clk_hz = u; n++; }
        if (get_u(NC_KEY_LED_BIT_CELLS, &u)) { led->bit_cells = (uint8_t)u; n++; }
        if (get_u(NC_KEY_LED_T0H, &u)) { led->t0h = (uint8_t)u; n++; }
        if (get_u(NC_KEY_LED_T1H, &u)) { led->t1h = (uint8_t)u; n++; }
    }
    return n;
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
