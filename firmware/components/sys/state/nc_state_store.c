// NCSTATE RAM store 實作（buffer 由 nc_state_boot 開機一次配置；store 本身唔 malloc）。
#include "nc_state_store.h"

#include <string.h>

bool nc_state_store_bind(nc_state_store_t *s, nc_state_ent_t *buf, uint16_t cap)
{
    if (s == NULL || buf == NULL || cap == 0 || cap > NC_STATE_STORE_MAX) {
        return false;
    }
    s->e = buf;
    s->n = 0;
    s->cap = cap;
    return true;
}

uint16_t nc_state_store_cap(const nc_state_store_t *s)
{
    return (s != NULL) ? s->cap : 0;
}

void nc_state_store_clear(nc_state_store_t *s)
{
    if (s != NULL) {
        s->n = 0;      // buffer 內容唔使清；n 以外嘅 entry 唔會被讀
    }
}

static nc_state_ent_t *find_mut(nc_state_store_t *s, uint32_t key)
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
    if (r->type == NC_STATE_T_BYTES) {
        return false;   // state 只存數值：字串請留喺 config／另設字串池（明示，唔截斷）
    }
    if (s->e == NULL || s->cap == 0) {
        return false;   // 未 bind（開機配置失敗）→ 明示失敗
    }
    nc_state_ent_t *e = find_mut(s, r->key);
    if (e == NULL) {
        if (s->n >= s->cap) {
            return false;
        }
        e = &s->e[s->n++];
        memset(e, 0, sizeof(*e));
    }
    e->key = r->key;
    e->type = r->type;
    e->u = r->u;    // union：sint 由下面覆寫
    if (r->type == NC_STATE_T_SINT) {
        e->i = r->i;
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

const nc_state_ent_t *nc_state_store_find(const nc_state_store_t *s, uint32_t key)
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

bool nc_state_store_get(const nc_state_store_t *s, uint32_t key, nc_state_rec_t *out)
{
    const nc_state_ent_t *e = nc_state_store_find(s, key);
    if (e == NULL || out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->key = e->key;
    out->type = e->type;
    out->u = e->u;      // union：同一個位
    out->i = e->i;
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
        return false;   // store 唔存字串（唔會相等）
    default:
        return false;
    }
}

uint16_t nc_state_store_prune(nc_state_store_t *s, const nc_state_rec_t *recs, size_t n)
{
    if (s == NULL) {
        return 0;
    }
    uint16_t keep = 0;
    for (uint16_t i = 0; i < s->n; i++) {
        // 動態域（0xF0xxxxxx）＝熱插拔任務／效果的值，唔屬 cfg 期望集，
        // 但佢哋係「開機要保留的 override」——所以 prune 唔可以掃走。
        bool found = ((s->e[i].key >> 24) == 0xF0u);
        for (size_t j = 0; j < n && !found; j++) {
            found = (recs[j].key == s->e[i].key);
        }
        if (found) {
            if (keep != i) {
                s->e[keep] = s->e[i];
            }
            keep++;
        }
    }
    const uint16_t dropped = (uint16_t)(s->n - keep);
    s->n = keep;
    return dropped;
}

uint32_t nc_state_store_cmp(const nc_state_store_t *s, const nc_state_rec_t *recs, size_t n,
                            uint32_t *first_diff)
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
