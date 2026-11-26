// NCSTATE 讀取 API 實作（由 state store 拿值）。
#include "nc_state_read.h"

#include <string.h>

#include "nc_state_blob.h"
#include "nc_state_boot.h"
#include "nc_state_store.h"

static const nc_state_store_t *store_of(void)
{
    const nc_state_store_t *st = nc_state_boot_store();
    if (st == NULL || st->n == 0) {
        return NULL;
    }
    return st;
}

bool nc_state_read_u32(uint16_t key, uint32_t *out)
{
    const nc_state_store_t *st = store_of();
    nc_state_rec_t r;
    if (st == NULL || out == NULL || !nc_state_store_get(st, key, &r)) {
        return false;
    }
    if (r.type == NC_STATE_T_UINT || r.type == NC_STATE_T_BOOL) {
        *out = r.u;
        return true;
    }
    return false;
}

bool nc_state_read_i32(uint16_t key, int32_t *out)
{
    const nc_state_store_t *st = store_of();
    nc_state_rec_t r;
    if (st == NULL || out == NULL || !nc_state_store_get(st, key, &r)) {
        return false;
    }
    if (r.type == NC_STATE_T_SINT) {
        *out = r.i;
        return true;
    }
    return false;
}

bool nc_state_read_bool(uint16_t key, bool *out)
{
    const nc_state_store_t *st = store_of();
    nc_state_rec_t r;
    if (st == NULL || out == NULL || !nc_state_store_get(st, key, &r)) {
        return false;
    }
    if (r.type == NC_STATE_T_BOOL) {
        *out = (r.u != 0);
        return true;
    }
    return false;
}

bool nc_state_read_str(uint16_t key, char *buf, size_t cap)
{
    const nc_state_store_t *st = store_of();
    nc_state_rec_t r;
    if (st == NULL || buf == NULL || cap == 0 ||
        !nc_state_store_get(st, key, &r)) {
        return false;
    }
    if (r.type != NC_STATE_T_BYTES) {
        return false;
    }
    if ((size_t)r.bytes_len >= cap) {
        return false;   // 唔截斷
    }
    if (r.bytes_len > 0 && r.bytes != NULL) {
        memcpy(buf, r.bytes, r.bytes_len);
    }
    buf[r.bytes_len] = '\0';
    return true;
}
