// NCSTATE 讀寫 API 實作：schema 查表 → RAM store。
//
// 呢個檔刻意保持純 C（唔 include esp_log／esp_timer），方便 host test 直接編。
// 讀寫都只經 nc_state_schema（表）＋nc_state_store（值），冇第三份名單。
#include "nc_state_api.h"

#include <string.h>

#include "nc_state_blob.h"
#include "nc_state_boot.h"
#include "nc_state_schema.h"
#include "nc_state_store.h"

// ── store（boot 建立；未 sync 前 NULL） ─────────────────────────
static const nc_state_store_t *store_ro(void)
{
    const nc_state_store_t *st = nc_state_boot_store();
    return (st != NULL && st->n > 0) ? st : NULL;
}

// ── dirty：今次開機改過邊幾個 key（固定陣列，無 heap） ────────────
static uint32_t s_dirty_keys[NC_STATE_STORE_MAX];
static uint16_t s_dirty_n;

static void dirty_mark(uint32_t key)
{
    for (uint16_t i = 0; i < s_dirty_n; i++) {
        if (s_dirty_keys[i] == key) {
            return;
        }
    }
    if (s_dirty_n < NC_STATE_STORE_MAX) {
        s_dirty_keys[s_dirty_n++] = key;
    }
}

uint16_t nc_state_dirty_count(void)
{
    return s_dirty_n;
}

void nc_state_dirty_reset(void)
{
    s_dirty_n = 0;
}

// ── activation（S4）────────────────────────────────────────────
static uint16_t s_active = 0xFFFFu;

void nc_state_active_units_set(uint16_t mask)
{
    s_active = mask;
}

uint16_t nc_state_active_units(void)
{
    return s_active;
}

bool nc_state_unit_active(uint8_t unit)
{
    return unit < 16u && (s_active & (1u << unit)) != 0;
}

// ── 讀 ─────────────────────────────────────────────────────────
bool nc_state_get_u32(uint32_t key, uint32_t *out)
{
    const nc_state_keydef_t *def = nc_state_key(key);
    if (def == NULL || out == NULL || def->type != NC_STATE_T_UINT) {
        return false;
    }
    nc_state_rec_t r;
    if (!nc_state_store_get(store_ro(), key, &r) || r.type != NC_STATE_T_UINT) {
        return false;
    }
    *out = r.u;
    return true;
}

bool nc_state_get_i32(uint32_t key, int32_t *out)
{
    const nc_state_keydef_t *def = nc_state_key(key);
    if (def == NULL || out == NULL || def->type != NC_STATE_T_SINT) {
        return false;
    }
    nc_state_rec_t r;
    if (!nc_state_store_get(store_ro(), key, &r) || r.type != NC_STATE_T_SINT) {
        return false;
    }
    *out = r.i;
    return true;
}

bool nc_state_get_bool(uint32_t key, bool *out)
{
    const nc_state_keydef_t *def = nc_state_key(key);
    if (def == NULL || out == NULL || def->type != NC_STATE_T_BOOL) {
        return false;
    }
    nc_state_rec_t r;
    if (!nc_state_store_get(store_ro(), key, &r) || r.type != NC_STATE_T_BOOL) {
        return false;
    }
    *out = (r.u != 0);
    return true;
}

// ── 熱路徑綁定 ─────────────────────────────────────────────────
bool nc_state_bind_u32(uint32_t key, const uint32_t **out)
{
    const nc_state_keydef_t *def = nc_state_key(key);
    const nc_state_store_t *st = store_ro();
    if (def == NULL || out == NULL || st == NULL || def->type != NC_STATE_T_UINT) {
        return false;
    }
    const nc_state_ent_t *e = nc_state_store_find(st, key);
    if (e == NULL || e->type != NC_STATE_T_UINT) {
        return false;
    }
    *out = &e->u;
    return true;
}

bool nc_state_bind_i32(uint32_t key, const int32_t **out)
{
    const nc_state_keydef_t *def = nc_state_key(key);
    const nc_state_store_t *st = store_ro();
    if (def == NULL || out == NULL || st == NULL || def->type != NC_STATE_T_SINT) {
        return false;
    }
    const nc_state_ent_t *e = nc_state_store_find(st, key);
    if (e == NULL || e->type != NC_STATE_T_SINT) {
        return false;
    }
    *out = &e->i;
    return true;
}

// ── 寫 ─────────────────────────────────────────────────────────
const char *nc_state_set_res_name(nc_state_set_res_t r)
{
    switch (r) {
    case NC_STATE_SET_OK:           return "ok";
    case NC_STATE_SET_UNKNOWN_KEY:  return "unknown_key";
    case NC_STATE_SET_WRONG_TYPE:   return "wrong_type";
    case NC_STATE_SET_OUT_OF_RANGE: return "out_of_range";
    case NC_STATE_SET_NOT_READY:    return "not_ready";
    case NC_STATE_SET_NO_SPACE:     return "no_space";
    case NC_STATE_SET_NOT_ACTIVE:   return "not_active";
    case NC_STATE_SET_INVALID_ARG:  return "invalid_arg";
    default:                        return "?";
    }
}

static nc_state_store_t *store_rw(void)
{
    return nc_state_boot_store_mut();
}

static nc_state_set_res_t put_rec(const nc_state_keydef_t *def, const nc_state_rec_t *r)
{
    nc_state_store_t *st = store_rw();
    if (st == NULL) {
        return NC_STATE_SET_NOT_READY;
    }
    if (!nc_state_unit_active(def->unit)) {
        return NC_STATE_SET_NOT_ACTIVE;
    }
    if (!nc_state_store_put(st, r)) {
        return NC_STATE_SET_NO_SPACE;
    }
    dirty_mark(def->id);
    return NC_STATE_SET_OK;
}

uint32_t nc_state_name_hash(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return 0;
    }
    uint32_t h = 2166136261u;                 // FNV-1a：細、穩定、唔使表
    for (const unsigned char *p = (const unsigned char *)name; *p != '\0'; p++) {
        h ^= *p;
        h *= 16777619u;
    }
    return h;
}

bool nc_state_key_is_dyn(uint32_t key)
{
    return (key >> 24) == NC_STATE_DYN_DOMAIN;
}

static uint32_t dyn_key(const char *name)
{
    return ((uint32_t)NC_STATE_DYN_DOMAIN << 24) | (nc_state_name_hash(name) & 0x00FFFFFFu);
}

nc_state_set_res_t nc_state_dyn_set_s32(const char *name, int32_t v)
{
    if (name == NULL || name[0] == '\0') {
        return NC_STATE_SET_INVALID_ARG;
    }
    nc_state_store_t *st = store_rw();
    if (st == NULL) {
        return NC_STATE_SET_NOT_READY;
    }
    const uint32_t key = dyn_key(name);
    const nc_state_rec_t r = { .key = key, .type = NC_STATE_T_SINT, .i = v };
    if (!nc_state_store_put(st, &r)) {
        return NC_STATE_SET_NO_SPACE;
    }
    dirty_mark(key);
    return NC_STATE_SET_OK;
}

bool nc_state_dyn_get_s32(const char *name, int32_t *v)
{
    if (name == NULL || name[0] == '\0' || v == NULL) {
        return false;
    }
    nc_state_rec_t r;
    const nc_state_store_t *st = nc_state_boot_store();
    if (st == NULL || !nc_state_store_get(st, dyn_key(name), &r) || r.type != NC_STATE_T_SINT) {
        return false;
    }
    *v = r.i;
    return true;
}

nc_state_set_res_t nc_state_set_u32(uint32_t key, uint32_t v)
{
    const nc_state_keydef_t *def = nc_state_key(key);
    if (def == NULL) {
        return NC_STATE_SET_UNKNOWN_KEY;
    }
    if (def->type != NC_STATE_T_UINT) {
        return NC_STATE_SET_WRONG_TYPE;
    }
    if (!nc_state_key_in_range(def, (int64_t)v)) {
        return NC_STATE_SET_OUT_OF_RANGE;
    }
    const nc_state_rec_t r = { .key = key, .type = NC_STATE_T_UINT, .u = v };
    return put_rec(def, &r);
}

nc_state_set_res_t nc_state_set_i32(uint32_t key, int32_t v)
{
    const nc_state_keydef_t *def = nc_state_key(key);
    if (def == NULL) {
        return NC_STATE_SET_UNKNOWN_KEY;
    }
    if (def->type != NC_STATE_T_SINT) {
        return NC_STATE_SET_WRONG_TYPE;
    }
    if (!nc_state_key_in_range(def, (int64_t)v)) {
        return NC_STATE_SET_OUT_OF_RANGE;
    }
    const nc_state_rec_t r = { .key = key, .type = NC_STATE_T_SINT, .i = v };
    return put_rec(def, &r);
}

nc_state_set_res_t nc_state_set_bool(uint32_t key, bool v)
{
    const nc_state_keydef_t *def = nc_state_key(key);
    if (def == NULL) {
        return NC_STATE_SET_UNKNOWN_KEY;
    }
    if (def->type != NC_STATE_T_BOOL) {
        return NC_STATE_SET_WRONG_TYPE;
    }
    if (!nc_state_key_in_range(def, v ? 1 : 0)) {
        return NC_STATE_SET_OUT_OF_RANGE;
    }
    const nc_state_rec_t r = { .key = key, .type = NC_STATE_T_BOOL, .u = v ? 1u : 0u };
    return put_rec(def, &r);
}

