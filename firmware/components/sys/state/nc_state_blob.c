// NCSTATE codec：唯一格式（統一＋內部自描述）。見 nc_state_blob.h。
#include "nc_state_blob.h"

#include <string.h>

static const uint8_t k_magic[NC_STATE_BLOB_MAGIC_BYTES] = {'N','C','S','T','A','T','E', 0};

static uint32_t crc32_bytes(const uint8_t *p, size_t n)
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

static size_t varint_len(uint32_t v)
{
    size_t n = 1;
    while (v > 0x7Fu) {
        v >>= 7;
        n++;
    }
    return n;
}

static size_t put_varint(uint8_t *p, uint32_t v)
{
    size_t n = 0;
    while (v > 0x7Fu) {
        p[n++] = (uint8_t)((v & 0x7Fu) | 0x80u);
        v >>= 7;
    }
    p[n++] = (uint8_t)v;
    return n;
}

static uint32_t zigzag(int32_t v)
{
    return ((uint32_t)v << 1) ^ (uint32_t)(v >> 31);
}

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

// 值佔幾多 byte（唔包 type/key/len 前綴）；invalid 回 0。
static size_t rec_value_len(const nc_state_rec_t *r)
{
    switch (r->type) {
    case NC_STATE_T_UINT:
        return varint_len(r->u);
    case NC_STATE_T_SINT:
        return varint_len(zigzag(r->i));
    case NC_STATE_T_BOOL:
        return 1;
    case NC_STATE_T_BYTES:
        return r->bytes_len;
    default:
        return 0;
    }
}

// ── 值的 CRC（cache 失效判斷用）────────────────────────────────
// 同 blob payload 的 record 編碼一致（type＋key＋len＋value），但唔含 header。
// 為什麼唔用 JSON 檔案 bytes：改一句 _comment 就會令 hash 變 → 白白清掉回寫值。
static uint32_t crc32_upd(uint32_t crc, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return crc;
}

uint32_t nc_state_blob_rec_crc(const nc_state_rec_t *recs, size_t nrec)
{
    if (nrec > 0 && recs == NULL) {
        return 0;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < nrec; i++) {
        const nc_state_rec_t *r = &recs[i];
        const size_t vlen = rec_value_len(r);
        if (vlen == 0 && r->type != NC_STATE_T_BYTES) {
            return 0;   // 非法值 → 0（呼叫端要當錯，唔好當「相同」）
        }
        uint8_t pre[1 + 5 + 5];   // type＋key varint（≤5）＋len varint（≤5）
        size_t n = 0;
        pre[n++] = r->type;
        n += put_varint(&pre[n], r->key);
        n += put_varint(&pre[n], (uint32_t)vlen);
        crc = crc32_upd(crc, pre, n);

        uint8_t vb[5];
        switch (r->type) {
        case NC_STATE_T_UINT:
            crc = crc32_upd(crc, vb, put_varint(vb, r->u));
            break;
        case NC_STATE_T_SINT:
            crc = crc32_upd(crc, vb, put_varint(vb, zigzag(r->i)));
            break;
        case NC_STATE_T_BOOL: {
            const uint8_t b = (uint8_t)(r->u ? 1u : 0u);
            crc = crc32_upd(crc, &b, 1);
            break;
        }
        case NC_STATE_T_BYTES:
            if (r->bytes_len > 0 && r->bytes != NULL) {
                crc = crc32_upd(crc, (const uint8_t *)r->bytes, r->bytes_len);
            }
            break;
        default:
            return 0;
        }
    }
    return ~crc;
}

size_t nc_state_blob_size(const nc_state_rec_t *recs, size_t nrec)
{
    if (nrec > 0xFFFFu || (nrec > 0 && recs == NULL)) {
        return 0;
    }
    size_t total = NC_STATE_BLOB_HDR;
    for (size_t i = 0; i < nrec; i++) {
        const nc_state_rec_t *r = &recs[i];
        const size_t vlen = rec_value_len(r);
        if (vlen == 0 && r->type != NC_STATE_T_BYTES) {
            return 0;   // 型別唔識／bytes_len 0 以外嘅 0 值代表非法
        }
        if (r->type == NC_STATE_T_BYTES && r->bytes_len == 0 && r->bytes == NULL) {
            return 0;   // 明確：0 長度都要有個合法指標（NULL 當錯）
        }
        total += 1 + varint_len(r->key) + varint_len((uint32_t)vlen) + vlen;
    }
    return total;
}

size_t nc_state_blob_encode(uint8_t *out, size_t cap, const nc_state_meta_t *meta,
                            const nc_state_rec_t *recs, size_t nrec)
{
    if (out == NULL || meta == NULL) {
        return 0;
    }
    const size_t need = nc_state_blob_size(recs, nrec);
    if (need == 0 || need > cap) {
        return 0;
    }

    size_t pos = NC_STATE_BLOB_HDR;
    for (size_t i = 0; i < nrec; i++) {
        const nc_state_rec_t *r = &recs[i];
        out[pos++] = r->type;
        pos += put_varint(&out[pos], r->key);
        const size_t vlen = rec_value_len(r);
        pos += put_varint(&out[pos], (uint32_t)vlen);
        switch (r->type) {
        case NC_STATE_T_UINT:
            pos += put_varint(&out[pos], r->u);
            break;
        case NC_STATE_T_SINT:
            pos += put_varint(&out[pos], zigzag(r->i));
            break;
        case NC_STATE_T_BOOL:
            out[pos++] = (uint8_t)(r->u ? 1u : 0u);
            break;
        case NC_STATE_T_BYTES:
            if (r->bytes_len > 0) {
                memcpy(&out[pos], r->bytes, r->bytes_len);
                pos += r->bytes_len;
            }
            break;
        default:
            return 0;   // 上面 size() 已擋，理論上唔到
        }
    }

    memcpy(&out[0], k_magic, NC_STATE_BLOB_MAGIC_BYTES);
    put_u16(&out[8], meta->ver);
    put_u16(&out[10], meta->flags);
    put_u16(&out[12], (uint16_t)nrec);
    put_u32(&out[14], meta->src_hash);
    put_u32(&out[18], (uint32_t)(pos - NC_STATE_BLOB_HDR));
    put_u32(&out[22], crc32_bytes(&out[NC_STATE_BLOB_HDR], pos - NC_STATE_BLOB_HDR));
    return pos;
}

// 從 [p, p+len) 讀一個 varint，成功回 consumed（1..5），失敗回 0。
static size_t get_varint(const uint8_t *p, size_t len, uint32_t *out)
{
    uint32_t v = 0;
    for (size_t i = 0; i < len && i < 5; i++) {
        const uint8_t b = p[i];
        v |= (uint32_t)(b & 0x7Fu) << (7u * i);
        if ((b & 0x80u) == 0) {
            *out = v;
            return i + 1;
        }
    }
    return 0;
}

esp_err_t nc_state_blob_decode(const uint8_t *buf, size_t len,
                               nc_state_rec_cb_t cb, void *ud,
                               nc_state_info_t *info)
{
    if (buf == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (info != NULL) {
        memset(info, 0, sizeof(*info));
    }
    if (len < NC_STATE_BLOB_HDR) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (memcmp(&buf[0], k_magic, NC_STATE_BLOB_MAGIC_BYTES) != 0) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint16_t ver = get_u16(&buf[8]);
    const uint16_t flags = get_u16(&buf[10]);
    const uint16_t count = get_u16(&buf[12]);
    const uint32_t src_hash = get_u32(&buf[14]);
    const uint32_t payload_len = get_u32(&buf[18]);
    const uint32_t crc = get_u32(&buf[22]);
    if (ver != NC_STATE_BLOB_VER) {
        return ESP_ERR_INVALID_STATE;
    }
    if (payload_len != len - NC_STATE_BLOB_HDR) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (crc32_bytes(&buf[NC_STATE_BLOB_HDR], payload_len) != crc) {
        return ESP_FAIL;
    }

    size_t pos = NC_STATE_BLOB_HDR;
    const size_t end = (size_t)NC_STATE_BLOB_HDR + payload_len;
    uint32_t skipped = 0;
    for (uint16_t i = 0; i < count; i++) {
        if (pos >= end) {
            return ESP_ERR_INVALID_SIZE;
        }
        const uint8_t type = buf[pos++];
        uint32_t key = 0, vlen = 0;
        size_t n = get_varint(&buf[pos], end - pos, &key);
        if (n == 0) {
            return ESP_ERR_INVALID_SIZE;
        }
        pos += n;
        n = get_varint(&buf[pos], end - pos, &vlen);
        if (n == 0) {
            return ESP_ERR_INVALID_SIZE;
        }
        pos += n;
        if (vlen > end - pos) {
            return ESP_ERR_INVALID_SIZE;
        }
        nc_state_rec_t rec;
        memset(&rec, 0, sizeof(rec));
        rec.key = key;
        rec.type = type;
        switch (type) {
        case NC_STATE_T_UINT: {
            uint32_t v = 0;
            const size_t c = get_varint(&buf[pos], vlen, &v);
            if (c == 0 || c != vlen) {
                return ESP_ERR_INVALID_SIZE;
            }
            rec.u = v;
            break;
        }
        case NC_STATE_T_SINT: {
            uint32_t z = 0;
            const size_t c = get_varint(&buf[pos], vlen, &z);
            if (c == 0 || c != vlen) {
                return ESP_ERR_INVALID_SIZE;
            }
            rec.i = (int32_t)((z >> 1) ^ (uint32_t)(-(int32_t)(z & 1u)));
            break;
        }
        case NC_STATE_T_BOOL:
            if (vlen != 1 || (buf[pos] != 0 && buf[pos] != 1)) {
                return ESP_ERR_INVALID_SIZE;
            }
            rec.u = buf[pos];
            break;
        case NC_STATE_T_BYTES:
            rec.bytes = vlen > 0 ? (const void *)&buf[pos] : NULL;
            rec.bytes_len = (uint16_t)vlen;
            break;
        default:
            skipped++;      // 未知 type：靠 len 跳過（向前兼容）
            break;
        }
        if (type == NC_STATE_T_UINT || type == NC_STATE_T_SINT ||
            type == NC_STATE_T_BOOL || type == NC_STATE_T_BYTES) {
            if (cb != NULL) {
                const esp_err_t e = cb(ud, &rec);
                if (e != ESP_OK) {
                    return e;
                }
            }
        }
        pos += vlen;
    }
    if (pos != end) {
        return ESP_ERR_INVALID_SIZE;    // 有尾垃圾 → 當壞檔
    }
    if (info != NULL) {
        info->ver = ver;
        info->flags = flags;
        info->src_hash = src_hash;
        info->n_records = count;
        info->n_skipped = skipped;
    }
    return ESP_OK;
}
