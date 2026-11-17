#include "nc_meta.h"

#include <string.h>

static void copy_str(char *dst, size_t cap, const char *src, size_t src_len)
{
    size_t n = 0;
    while (n < src_len && n + 1 < cap && src[n] != '\0') {
        dst[n] = src[n];
        n++;
    }
    dst[n] = '\0';
}

bool nc_meta_parse(const void *buf, size_t len, nc_meta_t *out)
{
    if (buf == NULL || out == NULL || len < NC_META_SIZE) {
        return false;
    }
    const uint8_t *p = (const uint8_t *)buf;
    memset(out, 0, sizeof(*out));

    copy_str(out->name, sizeof(out->name), (const char *)p, NC_META_NAME_MAX);
    copy_str(out->desc, sizeof(out->desc), (const char *)(p + 24), NC_META_DESC_MAX);
    copy_str(out->usage, sizeof(out->usage), (const char *)(p + 88), NC_META_USAGE_MAX);

    out->n_requires = p[152];
    out->n_provides = p[153];
    if (out->n_requires > NC_META_MAX_REQ) {
        out->n_requires = NC_META_MAX_REQ;
    }
    if (out->n_provides > NC_META_MAX_PROV) {
        out->n_provides = NC_META_MAX_PROV;
    }

    for (int i = 0; i < out->n_requires; i++) {
        const uint8_t *e = p + 156 + i * NC_META_ENTRY_SIZE;
        out->requires[i].type = e[0];
        out->requires[i].flags = e[1];
        out->requires[i].bytes = (uint16_t)(e[2] | (e[3] << 8));
        copy_str(out->requires[i].name, sizeof(out->requires[i].name),
                 (const char *)(e + 4), 20);
    }
    for (int i = 0; i < out->n_provides; i++) {
        const uint8_t *e = p + 156 + NC_META_MAX_REQ * NC_META_ENTRY_SIZE +
                           i * NC_META_ENTRY_SIZE;
        out->provides[i].type = e[0];
        out->provides[i].flags = e[1];
        out->provides[i].bytes = (uint16_t)(e[2] | (e[3] << 8));
        copy_str(out->provides[i].name, sizeof(out->provides[i].name),
                 (const char *)(e + 4), 20);
    }
    return true;
}
