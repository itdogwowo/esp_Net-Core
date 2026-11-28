#include "nc_px_name_dict.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#endif

#define ND_MAGIC "NCNAMES\0"
#define ND_VER   1u
#define ND_EMPTY 0xFFFFFFFFu

typedef struct {
    uint32_t id;
    uint32_t off;       // into bytes pool
    uint16_t len;
    uint16_t pad;
} nd_rec_t;

struct nc_px_name_dict {
    nd_rec_t *rec;
    char     *pool;     // unique UTF-8 bytes，連續
    uint32_t  n;
    uint32_t  cap;
    uint32_t  pool_used;
    uint32_t  pool_cap;
    uint32_t *slot;     // hash -> rec index；ND_EMPTY = 空
    uint32_t  slot_cap;
    bool      dirty;
};

static void *nd_alloc(size_t n)
{
#if defined(ESP_PLATFORM)
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p != NULL) {
        return p;
    }
#endif
    return malloc(n);
}

static uint32_t nd_hash(const char *s, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h ^= (uint8_t)s[i];
        h *= 16777619u;
    }
    return h;
}

static bool nd_hash_build(nc_px_name_dict_t *d, uint32_t want)
{
    uint32_t cap = 16;
    while (cap < want * 2u) {
        cap <<= 1;
    }
    uint32_t *slot = (uint32_t *)nd_alloc((size_t)cap * sizeof(*slot));
    if (slot == NULL) {
        return false;
    }
    for (uint32_t i = 0; i < cap; i++) {
        slot[i] = ND_EMPTY;
    }
    for (uint32_t i = 0; i < d->n; i++) {
        const nd_rec_t *r = &d->rec[i];
        uint32_t pos = nd_hash(d->pool + r->off, r->len) & (cap - 1u);
        while (slot[pos] != ND_EMPTY) {
            pos = (pos + 1u) & (cap - 1u);
        }
        slot[pos] = i;
    }
    free(d->slot);
    d->slot = slot;
    d->slot_cap = cap;
    return true;
}

static bool nd_reserve(nc_px_name_dict_t *d, uint32_t need)
{
    if (need <= d->cap) {
        return true;
    }
    uint32_t cap = (d->cap != 0) ? d->cap : 16u;
    while (cap < need) {
        cap *= 2u;
    }
    nd_rec_t *r = (nd_rec_t *)nd_alloc((size_t)cap * sizeof(*r));
    if (r == NULL) {
        return false;
    }
    if (d->rec != NULL && d->n > 0) {
        memcpy(r, d->rec, (size_t)d->n * sizeof(*r));
    }
    free(d->rec);
    d->rec = r;
    d->cap = cap;
    return true;
}

static uint32_t nd_append(nc_px_name_dict_t *d, const char *name, size_t len)
{
    if (d == NULL || name == NULL || len == 0 || len > 0xFFFFu) {
        return NC_PX_NAME_DICT_BAD_ID;
    }
    if (!nd_reserve(d, d->n + 1u)) {
        return NC_PX_NAME_DICT_BAD_ID;
    }
    if (d->pool_used + len + 1u > d->pool_cap) {
        uint32_t cap = (d->pool_cap != 0) ? d->pool_cap : 256u;
        while (cap < d->pool_used + len + 1u) {
            cap *= 2u;
        }
        char *p = (char *)nd_alloc(cap);
        if (p == NULL) {
            return NC_PX_NAME_DICT_BAD_ID;
        }
        if (d->pool != NULL && d->pool_used > 0) {
            memcpy(p, d->pool, d->pool_used);
        }
        free(d->pool);
        d->pool = p;
        d->pool_cap = cap;
    }
    const uint32_t off = d->pool_used;
    memcpy(d->pool + off, name, len);
    d->pool[off + len] = '\0';
    d->pool_used += (uint32_t)len + 1u;
    const uint32_t id = d->n;
    d->rec[id].id = id;
    d->rec[id].off = off;
    d->rec[id].len = (uint16_t)len;
    d->n++;
    if (d->n * 2u >= d->slot_cap) {
        (void)nd_hash_build(d, d->n);
    } else {
        uint32_t pos = nd_hash(name, len) & (d->slot_cap - 1u);
        while (d->slot[pos] != ND_EMPTY) {
            pos = (pos + 1u) & (d->slot_cap - 1u);
        }
        d->slot[pos] = id;
    }
    d->dirty = true;
    return id;
}

nc_px_name_dict_t *nc_px_name_dict_create(void)
{
    nc_px_name_dict_t *d = (nc_px_name_dict_t *)nd_alloc(sizeof(*d));
    if (d == NULL) {
        return NULL;
    }
    memset(d, 0, sizeof(*d));
    (void)nd_hash_build(d, 0);
    return d;
}

void nc_px_name_dict_destroy(nc_px_name_dict_t *d)
{
    if (d == NULL) {
        return;
    }
    free(d->rec);
    free(d->pool);
    free(d->slot);
    free(d);
}

uint32_t nc_px_name_dict_find(nc_px_name_dict_t *d, const char *name, size_t len)
{
    if (d == NULL || name == NULL || len == 0 || d->slot_cap == 0) {
        return NC_PX_NAME_DICT_BAD_ID;
    }
    uint32_t pos = nd_hash(name, len) & (d->slot_cap - 1u);
    while (d->slot[pos] != ND_EMPTY) {
        const nd_rec_t *r = &d->rec[d->slot[pos]];
        if (r->len == len && memcmp(d->pool + r->off, name, len) == 0) {
            return r->id;
        }
        pos = (pos + 1u) & (d->slot_cap - 1u);
    }
    return NC_PX_NAME_DICT_BAD_ID;
}

uint32_t nc_px_name_dict_intern(nc_px_name_dict_t *d, const char *name, size_t len,
                                bool *added)
{
    if (added != NULL) {
        *added = false;
    }
    const uint32_t id = nc_px_name_dict_find(d, name, len);
    if (id != NC_PX_NAME_DICT_BAD_ID) {
        return id;
    }
    const uint32_t nid = nd_append(d, name, len);
    if (added != NULL) {
        *added = (nid != NC_PX_NAME_DICT_BAD_ID);
    }
    return nid;
}

uint32_t nc_px_name_dict_intern_id(nc_px_name_dict_t *d, uint32_t id,
                                   const char *name, size_t len,
                                   bool *added, bool *updated)
{
    if (added != NULL) {
        *added = false;
    }
    if (updated != NULL) {
        *updated = false;
    }
    if (d == NULL || name == NULL || len == 0 || len > 0xFFFFu) {
        return NC_PX_NAME_DICT_BAD_ID;
    }
    if (id < d->n && d->rec[id].id == id) {
        const nd_rec_t *r = &d->rec[id];
        if (r->len == len && memcmp(d->pool + r->off, name, len) == 0) {
            return id;
        }
        // 同一個 id，實際 name 同實時 name 唔同 → 直接覆蓋（保留 id）
        if (d->pool_used + len + 1u > d->pool_cap) {
            uint32_t cap = d->pool_cap ? d->pool_cap : 256u;
            while (cap < d->pool_used + len + 1u) {
                cap *= 2u;
            }
            char *np = (char *)nd_alloc(cap);
            if (np == NULL) {
                return NC_PX_NAME_DICT_BAD_ID;
            }
            if (d->pool != NULL && d->pool_used > 0) {
                memcpy(np, d->pool, d->pool_used);
            }
            free(d->pool);
            d->pool = np;
            d->pool_cap = cap;
        }
        d->rec[id].off = d->pool_used;
        d->rec[id].len = (uint16_t)len;
        memcpy(d->pool + d->pool_used, name, len);
        d->pool[d->pool_used + len] = '\0';
        d->pool_used += (uint32_t)len + 1u;
        d->dirty = true;
        (void)nd_hash_build(d, d->n);
        if (updated != NULL) {
            *updated = true;
        }
        return id;
    }
    return nc_px_name_dict_intern(d, name, len, added);
}

#define ND_HDR_TAG "NCNAMES\0"
#define ND_HDR_VER 1u

typedef struct {
    char     tag[8];
    uint16_t ver;
    uint16_t flags;
    uint32_t count;
    uint32_t strings_bytes;
    uint32_t payload_crc;
} nd_hdr_t;

static uint32_t nd_crc32(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return ~crc;
}

esp_err_t nc_px_name_dict_load(nc_px_name_dict_t *d, const char *path)
{
    if (d == NULL || path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    nd_hdr_t h;
    if (fread(&h, sizeof(h), 1, f) != 1 || memcmp(h.tag, ND_HDR_TAG, sizeof(h.tag)) != 0 ||
        h.ver != ND_HDR_VER) {
        fclose(f);
        return ESP_ERR_INVALID_STATE;
    }
    if (h.count == 0) {
        fclose(f);
        return ESP_ERR_INVALID_STATE;
    }
    const size_t want = (size_t)h.strings_bytes;
    uint8_t *payload = (uint8_t *)nd_alloc(want ? want : 1u);
    if (payload == NULL) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    if (fread(payload, 1, want, f) != want) {
        free(payload);
        fclose(f);
        return ESP_ERR_INVALID_STATE;
    }
    fclose(f);
    if (nd_crc32(payload, want) != h.payload_crc) {
        free(payload);
        return ESP_ERR_INVALID_STATE;
    }
    // 重建 entries：record = id u32 + len u16 + pad u16 + bytes
    nc_px_name_dict_t *nd = nc_px_name_dict_create();
    if (nd == NULL) {
        free(payload);
        return ESP_ERR_NO_MEM;
    }
    size_t off = 0;
    for (uint32_t i = 0; i < h.count; i++) {
        if (off + 8u > want) {
            free(payload);
            nc_px_name_dict_destroy(nd);
            return ESP_ERR_INVALID_STATE;
        }
        uint32_t id;
        uint16_t len;
        memcpy(&id, payload + off, 4);
        memcpy(&len, payload + off + 4, 2);
        off += 8;
        if (off + len > want || id != i) {
            free(payload);
            nc_px_name_dict_destroy(nd);
            return ESP_ERR_INVALID_STATE;
        }
        (void)nd_append(nd, (const char *)(payload + off), len);
        off += len;
    }
    free(payload);
    nd->dirty = false;
    // swap 入 caller 的 dict；舊內容釋放
    free(d->rec);
    free(d->pool);
    free(d->slot);
    *d = *nd;
    free(nd);
    return ESP_OK;
}

esp_err_t nc_px_name_dict_save(nc_px_name_dict_t *d, const char *path)
{
    if (d == NULL || path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t payload_len = 0;
    for (uint32_t i = 0; i < d->n; i++) {
        payload_len += 8u + d->rec[i].len;
    }
    uint8_t *payload = (uint8_t *)nd_alloc(payload_len ? payload_len : 1u);
    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }
    size_t off = 0;
    for (uint32_t i = 0; i < d->n; i++) {
        const uint32_t id = i;
        const uint16_t len = d->rec[i].len;
        const uint16_t zero = 0;
        memcpy(payload + off, &id, 4); off += 4;
        memcpy(payload + off, &len, 2); off += 2;
        memcpy(payload + off, &zero, 2); off += 2;
        memcpy(payload + off, d->pool + d->rec[i].off, len); off += len;
    }
    nd_hdr_t h;
    memset(&h, 0, sizeof(h));
    memcpy(h.tag, ND_HDR_TAG, sizeof(h.tag));
    h.ver = ND_HDR_VER;
    h.count = d->n;
    h.strings_bytes = (uint32_t)payload_len;   // 實際 payload bytes（唔係 pool bytes）
    h.payload_crc = nd_crc32(payload, payload_len);
    char tmp[512];
    if (snprintf(tmp, sizeof(tmp), "%s.part", path) >= (int)sizeof(tmp)) {
        free(payload);
        return ESP_ERR_INVALID_ARG;
    }
    FILE *f = fopen(tmp, "wb");
    if (f == NULL) {
        free(payload);
        return ESP_ERR_NOT_FOUND;
    }
    const bool ok = fwrite(&h, sizeof(h), 1, f) == 1 &&
                    fwrite(payload, 1, payload_len, f) == payload_len;
    fclose(f);
    if (!ok) {
        remove(tmp);
        free(payload);
        return ESP_ERR_INVALID_STATE;
    }
    remove(path);
    if (rename(tmp, path) != 0) {
        remove(tmp);
        free(payload);
        return ESP_ERR_INVALID_STATE;
    }
    free(payload);
    d->dirty = false;
    return ESP_OK;
}

const char *nc_px_name_dict_name(const nc_px_name_dict_t *d, uint32_t id, uint16_t *len)
{
    if (d == NULL || id >= d->n || d->rec[id].id != id) {
        return NULL;
    }
    if (len != NULL) {
        *len = d->rec[id].len;
    }
    return d->pool + d->rec[id].off;
}

uint32_t nc_px_name_dict_count(const nc_px_name_dict_t *d)
{
    return (d != NULL) ? d->n : 0;
}

bool nc_px_name_dict_dirty(const nc_px_name_dict_t *d)
{
    return (d != NULL) && d->dirty;
}

static nc_px_name_dict_t *s_global;

nc_px_name_dict_t *nc_px_name_dict_global(void)
{
    if (s_global == NULL) {
        s_global = nc_px_name_dict_create();
    }
    return s_global;
}

void nc_px_name_dict_global_reset(void)
{
    nc_px_name_dict_destroy(s_global);
    s_global = NULL;
}

uint32_t nc_px_name_dict_global_intern(const char *name, size_t len)
{
    return nc_px_name_dict_intern(nc_px_name_dict_global(), name, len, NULL);
}

const char *nc_px_name_dict_global_str(uint32_t id)
{
    const char *s = nc_px_name_dict_name(nc_px_name_dict_global(), id, NULL);
    return (s != NULL) ? s : "";
}

const char *nc_px_name_dict_global_intern_cstr(const char *name)
{
    if (name == NULL) {
        return "";
    }
    const uint32_t id = nc_px_name_dict_global_intern(name, strlen(name));
    return nc_px_name_dict_global_str(id);
}
