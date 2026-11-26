// NCSTATE schema 查詢／驗證實作（純 C；表由 nc_state_schema_data.c 生成）。
#include "nc_state_schema.h"

#include <string.h>

const nc_state_unitdef_t *nc_state_unit_at(uint16_t i)
{
    return (i < NC_STATE_UNIT_COUNT) ? &nc_state_schema_units[i] : NULL;
}

uint16_t nc_state_unit_count(void)
{
    return NC_STATE_UNIT_COUNT;
}

const nc_state_unitdef_t *nc_state_unit(uint8_t id)
{
    for (uint16_t i = 0; i < NC_STATE_UNIT_COUNT; i++) {
        if (nc_state_schema_units[i].id == id) {
            return &nc_state_schema_units[i];
        }
    }
    return NULL;
}

const nc_state_keydef_t *nc_state_key_at(uint16_t i)
{
    return (i < NC_STATE_KEY_COUNT) ? &nc_state_schema_keys[i] : NULL;
}

uint16_t nc_state_key_count(void)
{
    return NC_STATE_KEY_COUNT;
}

const nc_state_keydef_t *nc_state_key(uint16_t id)
{
    // 25 個 key、線性搜尋已足夠；表按 id 升序（生成器排序），可以早停。
    for (uint16_t i = 0; i < NC_STATE_KEY_COUNT; i++) {
        const nc_state_keydef_t *d = &nc_state_schema_keys[i];
        if (d->id == id) {
            return d;
        }
        if (d->id > id) {
            break;
        }
    }
    return NULL;
}

const nc_state_keydef_t *nc_state_key_name(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (uint16_t i = 0; i < NC_STATE_KEY_COUNT; i++) {
        if (strcmp(nc_state_schema_keys[i].name, name) == 0) {
            return &nc_state_schema_keys[i];
        }
    }
    return NULL;
}

const nc_state_unitdef_t *nc_state_unit_of(const nc_state_keydef_t *def)
{
    return (def != NULL) ? nc_state_unit(def->unit) : NULL;
}

const char *nc_state_type_name(uint8_t type)
{
    switch (type) {
    case NC_STATE_T_UINT:  return "uint";
    case NC_STATE_T_SINT:  return "sint";
    case NC_STATE_T_BYTES: return "bytes";
    case NC_STATE_T_BOOL:  return "bool";
    default:               return "?";
    }
}

const char *nc_state_enum_name(const nc_state_keydef_t *def, uint32_t v)
{
    if (def == NULL || def->n_enum == 0 || def->enum_names == NULL || v >= def->n_enum) {
        return NULL;
    }
    return def->enum_names[v];
}

bool nc_state_key_in_range(const nc_state_keydef_t *def, int64_t v)
{
    if (def == NULL) {
        return false;
    }
    return v >= (int64_t)def->min && v <= (int64_t)def->max;
}

bool nc_state_key_str_ok(const nc_state_keydef_t *def, size_t len)
{
    if (def == NULL || def->type != NC_STATE_T_BYTES) {
        return false;
    }
    return len <= (size_t)def->max_len;
}

uint8_t nc_state_schema_check(const nc_state_rec_t *rec)
{
    if (rec == NULL) {
        return NC_STATE_CHK_UNKNOWN_KEY;
    }
    const nc_state_keydef_t *def = nc_state_key(rec->key);
    if (def == NULL) {
        return NC_STATE_CHK_UNKNOWN_KEY;
    }
    if (def->type != rec->type) {
        return NC_STATE_CHK_TYPE;
    }
    switch (rec->type) {
    case NC_STATE_T_UINT:
    case NC_STATE_T_SINT: {
        const int64_t v = (rec->type == NC_STATE_T_UINT) ? (int64_t)rec->u : (int64_t)rec->i;
        return nc_state_key_in_range(def, v) ? NC_STATE_CHK_OK : NC_STATE_CHK_RANGE;
    }
    case NC_STATE_T_BOOL:
        return nc_state_key_in_range(def, (int64_t)rec->u) ? NC_STATE_CHK_OK
                                                            : NC_STATE_CHK_RANGE;
    case NC_STATE_T_BYTES: {
        // 要同 store 的容量一齊睇：schema 准但 store 放唔落，boot 一樣會失敗。
        if (!nc_state_key_str_ok(def, rec->bytes_len)) {
            return NC_STATE_CHK_RANGE;
        }
        return (rec->bytes_len > 0 && rec->bytes == NULL) ? NC_STATE_CHK_TYPE
                                                          : NC_STATE_CHK_OK;
    }
    default:
        return NC_STATE_CHK_TYPE;
    }
}

const char *nc_state_schema_check_name(uint8_t code)
{
    switch (code) {
    case NC_STATE_CHK_OK:          return "ok";
    case NC_STATE_CHK_UNKNOWN_KEY: return "unknown_key";
    case NC_STATE_CHK_TYPE:        return "type";
    case NC_STATE_CHK_RANGE:       return "range";
    default:                       return "?";
    }
}

uint16_t nc_state_schema_version(void)
{
    return NC_STATE_SCHEMA_VERSION;
}

uint32_t nc_state_schema_crc(void)
{
    return NC_STATE_SCHEMA_CRC;
}
