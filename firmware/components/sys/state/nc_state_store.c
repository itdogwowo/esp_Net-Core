// NCSTATE RAM store 實作（固定陣列、無 heap／PSRAM）。
#include "nc_state_store.h"

#include <string.h>

void nc_state_store_clear(nc_state_store_t *s)
{
    if (s != NULL) {
        memset(s, 0, sizeof(*s));
    }
}

static nc_state_ent_t *find_mut(nc_state_store_t *s, uint16_t key)
{
    for (uint16_t i = 0; i < s->n; i++) {
        if (s->e[i].key == key) {
            return &s->e[i];
        }
    }
    return NULL;
}

bool nc_state_store_put(nc_state_store_t *s, const nc_state_rec_t *r)
{
    if (s == NULL || r == NULL) {
        return false;
    }
    if (r->type == NC_STATE_T_BYTES && r->bytes_len > NC_STATE_STORE_BYTES_MAX) {
        return false;   // 明示失敗，唔截斷
    }
    nc_state_ent_t *e = find_mut(s, r->key);
    if (e == NULL) {
        if (s->n >= NC_STATE_STORE_MAX) {
            return false;
        }
        e = &s->e[s->n++];
        memset(e, 0, sizeof(*e));
    }
    e->key = r->key;
    e->type = r->type;
    e->u = r->u;
    e->i = r->i;
    e->bytes_len = 0;
    if (r->type == NC_STATE_T_BYTES) {
        if (r->bytes_len > 0 && r->bytes != NULL) {
            memcpy(e->bytes, r->bytes, r->bytes_len);
            e->bytes_len = (uint8_t)r->bytes_len;
        }
    }
    return true;
}

esp_err_t nc_state_store_cb(void *ud, const nc_state_rec_t *r)
{
    if (ud == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return nc_state_store_put((nc_state_store_t *)ud, r) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

uint16_t nc_state_store_from_recs(nc_state_store_t *s, const nc_state_rec_t *recs, size_t n)
{
    if (s == NULL) {
        return 0;
    }
    nc_state_store_clear(s);
    uint16_t ok = 0;
    for (size_t i = 0; i < n; i++) {
        if (nc_state_store_put(s, &recs[i])) {
            ok++;
        } else {
            break;
        }
    }
    return ok;
}

const nc_state_ent_t *nc_state_store_find(const nc_state_store_t *s, uint16_t key)
{
    if (s == NULL) {
        return NULL;
    }
    for (uint16_t i = 0; i < s->n; i++) {
        if (s->e[i].key == key) {
            return &s->e[i];
        }
    }
    return NULL;
}

bool nc_state_store_get(const nc_state_store_t *s, uint16_t key, nc_state_rec_t *out)
{
    const nc_state_ent_t *e = nc_state_store_find(s, key);
    if (e == NULL || out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->key = e->key;
    out->type = e->type;
    out->u = e->u;
    out->i = e->i;
    if (e->type == NC_STATE_T_BYTES && e->bytes_len > 0) {
        out->bytes = e->bytes;
        out->bytes_len = e->bytes_len;
    }
    return true;
}

static bool ent_same(const nc_state_ent_t *e, const nc_state_rec_t *r)
{
    if (e->type != r->type) {
        return false;
    }
    switch (r->type) {
    case NC_STATE_T_UINT:
    case NC_STATE_T_BOOL:
        return e->u == r->u;
    case NC_STATE_T_SINT:
        return e->i == r->i;
    case NC_STATE_T_BYTES:
        return e->bytes_len == r->bytes_len &&
               (r->bytes_len == 0 ||
                (r->bytes != NULL && memcmp(e->bytes, r->bytes, r->bytes_len) == 0));
    default:
        return false;
    }
}

uint32_t nc_state_store_cmp(const nc_state_store_t *s, const nc_state_rec_t *recs, size_t n,
                            uint16_t *first_diff)
{
    if (first_diff != NULL) {
        *first_diff = 0;
    }
    if (s == NULL || (n > 0 && recs == NULL)) {
        return 0xFFFFFFFFu;
    }
    uint32_t diff = 0;
    bool first = true;
    for (size_t i = 0; i < n; i++) {
        const nc_state_ent_t *e = nc_state_store_find(s, recs[i].key);
        if (e == NULL || !ent_same(e, &recs[i])) {
            diff++;
            if (first) {
                first = false;
                if (first_diff != NULL) {
                    *first_diff = recs[i].key;
                }
            }
        }
    }
    return diff;
}
