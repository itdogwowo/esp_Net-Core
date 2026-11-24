// nc_px_layout.c — 排版 → destination 順序寫入表（見 nc_px_layout.h）。
//
// 步驟：
//   1. mode entry：`group.sel[]` → 有序全域 index（反序／隔顆／重疊照舊項目語義）
//   2. entry 的 `range`（Python slice）在該 index 列表上再切一次
//   3. 每顆 index 丟入 dst 的 bucket：counting sort（O(px+n)，穩定 = 保持 entry 次序）
//   4. 掃 dst 0..px-1，合併連續段成 run {dst, src, count, job}
//
// 記憶體：暫存全部由本層 malloc（board 上優先 PSRAM）；編譯只在換場做一次。
//   熱路徑只讀 runs ＋ 每條值流。
#include "nc_px_layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#endif

static void *lalloc(size_t n)
{
    if (n == 0) {
        return NULL;
    }
#if defined(ESP_PLATFORM)
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p != NULL) {
        return p;
    }
#endif
    return malloc(n);
}

// 6 B 落點表專用：**只可以放 internal**。呢張表每顆都要讀一次（9,600 px ＝ 57.6 KB），
// 真機量到：放 PSRAM 反而**慢過** runs（0.60–1.05×，每幀 57.6 KB PSRAM 讀取頻寬），
// 放 internal 就快 1.0–2.1×（見 status §7.217）。所以唔做「internal 唔夠就落 PSRAM」的
// 靜默降級——寧願冇表（行 runs，就係舊行為），都唔好行一條比舊行為更慢的路。
static void *ialloc(size_t n)
{
    if (n == 0) {
        return NULL;
    }
#if defined(ESP_PLATFORM)
    return heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    return malloc(n);          // host 測試：冇 internal／PSRAM 之分
#endif
}

static void lfree(void *p)
{
    free(p);
}

static const nc_px_hw_inv_t *find_inv(const nc_px_hw_inv_t *inv, uint32_t ninv, uint8_t hw)
{
    for (uint32_t i = 0; i < ninv; i++) {
        if (inv[i].hw == hw) {
            return &inv[i];
        }
    }
    return NULL;
}

static const nc_px_asset_t *find_asset(const nc_px_asset_tbl_t *tbl, const char *name)
{
    if (tbl == NULL || name == NULL) {
        return NULL;
    }
    for (uint16_t i = 0; i < tbl->n; i++) {
        if (strcmp(tbl->item[i].name, name) == 0) {
            return &tbl->item[i];
        }
    }
    return NULL;
}

// Python slice 的 start/stop/step → index 數目（end 不含、負數由尾數、step 可負）
static uint32_t slice_count(int16_t s, int16_t st, int16_t step, uint32_t n)
{
    int64_t inc = (step == 0) ? 1 : step;
    const int64_t len = (int64_t)n;
    int64_t start;
    int64_t stop;
    if (inc > 0) {
        start = (s < 0) ? (int64_t)s + len : (int64_t)s;
        stop = (st == 0) ? len : ((st < 0) ? (int64_t)st + len : (int64_t)st);
        if (start < 0) {
            start = 0;
        }
        if (stop > len) {
            stop = len;
        }
        if (start >= stop) {
            return 0;
        }
        return (uint32_t)((stop - start + inc - 1) / inc);
    }
    start = (s == 0) ? (len - 1) : ((s < 0) ? (int64_t)s + len : (int64_t)s);
    stop = (st == 0) ? -1 : ((st < 0) ? (int64_t)st + len : (int64_t)st);
    if (start > len - 1) {
        start = len - 1;
    }
    if (start <= stop) {
        return 0;
    }
    return (uint32_t)((start - stop + (-inc) - 1) / (-inc));
}

static uint32_t slice_fill(int16_t s, int16_t st, int16_t step, uint32_t n,
                           uint32_t *out, uint32_t cap)
{
    const uint32_t want = slice_count(s, st, step, n);
    if (want == 0 || want > cap) {
        return 0;
    }
    int64_t inc = (step == 0) ? 1 : step;
    const int64_t len = (int64_t)n;
    uint32_t k = 0;
    if (inc > 0) {
        int64_t start = (s < 0) ? (int64_t)s + len : (int64_t)s;
        int64_t stop = (st == 0) ? len : ((st < 0) ? (int64_t)st + len : (int64_t)st);
        if (start < 0) {
            start = 0;
        }
        if (stop > len) {
            stop = len;
        }
        for (int64_t i = start; i < stop; i += inc) {
            out[k++] = (uint32_t)i;
        }
    } else {
        int64_t start = (s == 0) ? (len - 1) : ((s < 0) ? (int64_t)s + len : (int64_t)s);
        int64_t stop = (st == 0) ? -1 : ((st < 0) ? (int64_t)st + len : (int64_t)st);
        if (start > len - 1) {
            start = len - 1;
        }
        for (int64_t i = start; i > stop; i += inc) {
            out[k++] = (uint32_t)i;
        }
    }
    return k;
}

// 一個 sel 段 → 全域 index 數目／內容
static uint32_t sel_count(const nc_px_group_sel_t *sel, const nc_px_hw_inv_t *inv,
                          uint32_t ninv)
{
    const nc_px_hw_inv_t *slot = find_inv(inv, ninv, sel->hw);
    if (slot == NULL || slot->px == 0) {
        return 0;
    }
    int16_t s = 0;
    int16_t st = 0;
    int16_t step = 0;
    if (!nc_px_slice_parse(sel->slice, &s, &st, &step)) {
        return 0;
    }
    return slice_count(s, st, step, slot->px);
}

static uint32_t sel_fill(const nc_px_group_sel_t *sel, const nc_px_hw_inv_t *inv,
                         uint32_t ninv, uint32_t *out, uint32_t cap)
{
    const nc_px_hw_inv_t *slot = find_inv(inv, ninv, sel->hw);
    if (slot == NULL || slot->px == 0) {
        return 0;
    }
    int16_t s = 0;
    int16_t st = 0;
    int16_t step = 0;
    if (!nc_px_slice_parse(sel->slice, &s, &st, &step)) {
        return 0;
    }
    const uint32_t want = slice_count(s, st, step, slot->px);
    if (want == 0 || want > cap) {
        return 0;
    }
    // 先寫 local index，再加 base（避免修改喺 out 的 buffer 太大）
    uint32_t k = slice_fill(s, st, step, slot->px, out, cap);
    for (uint32_t i = 0; i < k; i++) {
        out[i] += slot->base;
    }
    return k;
}

// 一個 group 的 sel[] → 有序全域 index（依書寫次序拼接）；回傳實際數量
static uint32_t group_count(const nc_px_group_t *g, const nc_px_hw_inv_t *inv, uint32_t ninv)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < g->nsel; i++) {
        n += sel_count(&g->sel[i], inv, ninv);
    }
    return n;
}

static uint32_t group_fill(const nc_px_group_t *g, const nc_px_hw_inv_t *inv, uint32_t ninv,
                           uint32_t *out, uint32_t cap)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < g->nsel && n < cap; i++) {
        n += sel_fill(&g->sel[i], inv, ninv, out + n, cap - n);
    }
    return n;
}

// ── group 名 hash（open addressing）──
// 為什麼要：mode 條目引用 group 名，線性掃描係 O(entries × groups)；
// 500 組 × 500 條目 = 25 萬次 strcmp，而且名字在 PSRAM → 板上 build 64 ms。
// hash 之後 O(entries)，build 只受實際展開成本限制。
static uint32_t name_hash(const char *s)
{
    uint32_t h = 2166136261u;
    while (s != NULL && *s != '\0') {
        h ^= (uint8_t)(*s++);
        h *= 16777619u;
    }
    return h;
}

// 回傳 >= n 的最小 2 次方（最少 8）
static uint32_t pow2_at_least(uint32_t n)
{
    uint32_t c = 8;
    while (c < n) {
        c <<= 1;
    }
    return c;
}

// 建表：ht[i] = group index + 1（0 = 空）
static void hash_build(const nc_px_map_t *map, int32_t *ht, uint32_t cap)
{
    for (uint32_t i = 0; i < cap; i++) {
        ht[i] = 0;
    }
    for (uint32_t g = 0; g < map->ngroups; g++) {
        const char *nm = map->group[g].name;
        uint32_t pos = name_hash(nm) & (cap - 1u);
        for (uint32_t probe = 0; probe < cap; probe++) {
            if (ht[pos] == 0) {
                ht[pos] = (int32_t)(g + 1);
                break;
            }
            pos = (pos + 1u) & (cap - 1u);
        }
    }
}

static const nc_px_group_t *hash_find(const nc_px_map_t *map, const int32_t *ht, uint32_t cap,
                                      const char *ref)
{
    if (map == NULL || ht == NULL || ref == NULL) {
        return NULL;
    }
    const char *g = ref;
    const char *dot = strchr(ref, '.');
    if (dot != NULL && dot != ref) {
        g = dot + 1;
    }
    uint32_t pos = name_hash(g) & (cap - 1u);
    for (uint32_t probe = 0; probe < cap; probe++) {
        const int32_t idx = ht[pos];
        if (idx == 0) {
            return NULL;
        }
        if (strcmp(map->group[idx - 1].name, g) == 0) {
            return &map->group[idx - 1];
        }
        pos = (pos + 1u) & (cap - 1u);
    }
    return NULL;
}

// ═══════════════════════ build ═══════════════════════
esp_err_t nc_px_layout_build(const nc_px_mode_t *mode, const nc_px_map_t *map,
                             const nc_px_asset_tbl_t *assets,
                             const nc_px_hw_inv_t *inv, uint32_t ninv,
                             nc_px_layout_t *out)
{
    if (mode == NULL || map == NULL || assets == NULL || out == NULL || mode->nentry == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (ninv > NC_PX_HW_INV_MAX) {
        ninv = NC_PX_HW_INV_MAX;
    }
    uint32_t px = 0;
    for (uint32_t i = 0; i < ninv; i++) {
        const uint32_t end = inv[i].base + inv[i].px;
        if (end > px) {
            px = end;
        }
    }
    out->px = px;
    const uint32_t hcap = pow2_at_least(map->ngroups * 2u + 1u);
    int32_t *ht = (int32_t *)lalloc((size_t)hcap * sizeof(int32_t));
    if (ht == NULL) {
        return ESP_ERR_NO_MEM;
    }
    hash_build(map, ht, hcap);
    if (px == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    // ── 1. 解析 entry（group／asset／write／range），計 index 上界 ──
    typedef struct {
        const nc_px_group_t *group;
        const nc_px_asset_t *asset;
        uint8_t  write;
        bool     has_range;
        int16_t  start;
        int16_t  stop;
        int16_t  step;
        uint32_t off;          // 在 index pool 的位置
        uint32_t n;            // 實際展開數
    } ent_src_t;

    ent_src_t *src = (ent_src_t *)lalloc((size_t)mode->nentry * sizeof(ent_src_t));
    if (src == NULL) {
        return ESP_ERR_NO_MEM;
    }
    uint32_t nent = 0;
    uint32_t total_need = 0;
    for (uint32_t e = 0; e < mode->nentry; e++) {
        const nc_px_mode_entry_t *en = &mode->entry[e];
        const nc_px_asset_t *asset = find_asset(assets, en->effect);
        const nc_px_group_t *group = hash_find(map, ht, hcap, en->group);
        if (asset == NULL) {
            out->skipped++;
            out->skip_no_asset++;
            continue;
        }
        if (group == NULL) {
            out->skipped++;
            out->skip_no_group++;
            continue;
        }
        if (en->write >= NC_PX_WRITE_COUNT) {
            out->skipped++;
            out->skip_bad_write++;
            continue;
        }
        uint32_t need = group_count(group, inv, ninv);
        if (need == 0) {
            out->skipped++;
            out->skip_empty++;
            continue;
        }
        if (en->has_range) {
            need = slice_count(en->start, en->stop, en->step, need);
        }
        if (need == 0) {
            out->skipped++;
            out->skip_empty++;
            continue;
        }
        src[nent].group = group;
        src[nent].asset = asset;
        src[nent].write = en->write;
        src[nent].has_range = en->has_range;
        src[nent].start = en->start;
        src[nent].stop = en->stop;
        src[nent].step = en->step;
        src[nent].off = 0;
        src[nent].n = 0;
        total_need += need;
        nent++;
    }
    if (nent == 0) {
        lfree(src);
        lfree(ht);
        return ESP_ERR_NOT_FOUND;
    }

    // ── 1b. 數實際 distinct 效果（aprog 只配真正用到的數量）──
    uint32_t ndistinct = 0;
    for (uint32_t e = 0; e < nent; e++) {
        bool seen = false;
        for (uint32_t k = 0; k < e; k++) {
            if (src[k].asset == src[e].asset) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            ndistinct++;
        }
    }
    if (ndistinct > 65535u) {          // job.aprog 是 uint16：欄位上限
        lfree(src);
        lfree(ht);
        return ESP_ERR_INVALID_ARG;
    }
    if (ndistinct == 0) {
        ndistinct = 1;
    }

    // ── 2. 配記憶體 ──
    uint32_t *pool = (uint32_t *)lalloc((size_t)total_need * sizeof(uint32_t));
    uint32_t *tmp = (uint32_t *)lalloc((size_t)total_need * sizeof(uint32_t));
    uint32_t *counts = (uint32_t *)lalloc((size_t)px * sizeof(uint32_t));
    uint32_t *cursor = (uint32_t *)lalloc((size_t)px * sizeof(uint32_t));
    uint16_t *e_job = (uint16_t *)lalloc((size_t)total_need * sizeof(uint16_t));
    uint32_t *e_src = (uint32_t *)lalloc((size_t)total_need * sizeof(uint32_t));
    out->aprog = (nc_px_aprog_t *)lalloc((size_t)ndistinct * sizeof(nc_px_aprog_t));
    out->job = (nc_px_ljob_t *)lalloc((size_t)nent * sizeof(nc_px_ljob_t));
    out->run = (nc_px_lrun_t *)lalloc((size_t)total_need * sizeof(nc_px_lrun_t));
    if (pool == NULL || tmp == NULL || counts == NULL || cursor == NULL || e_job == NULL ||
        e_src == NULL || out->aprog == NULL || out->job == NULL || out->run == NULL) {
        lfree(pool);
        lfree(tmp);
        lfree(counts);
        lfree(cursor);
        lfree(e_job);
        lfree(e_src);
        lfree(src);
        lfree(ht);
        nc_px_layout_free(out);
        return ESP_ERR_NO_MEM;
    }
    memset(counts, 0, (size_t)px * sizeof(uint32_t));
    memset(out->aprog, 0, (size_t)ndistinct * sizeof(nc_px_aprog_t));

    // ── 3. 展開全部 entry（保留書寫次序）＋ 編譯 aprog ＋ 數 bucket ──
    uint32_t naprog = 0;
    uint32_t nvals = 0;
    uint32_t pool_used = 0;
    uint32_t kept = 0;
    for (uint32_t e = 0; e < nent; e++) {
        // aprog：同一個 asset 只編一次 program
        uint32_t ai = UINT32_MAX;
        for (uint32_t i = 0; i < naprog; i++) {
            if (out->aprog[i].asset == src[e].asset) {
                ai = i;
                break;
            }
        }
        if (ai == UINT32_MAX) {
            if (naprog >= ndistinct) {           // 正常唔會發生（已按實際數量配置）

                out->skipped++;
                out->skip_asset_full++;
                continue;
            }
            ai = naprog;
            out->aprog[ai].asset = src[e].asset;
            out->aprog[ai].nv = src[e].asset->pixel_hint ? src[e].asset->pixel_hint : 1;
            out->aprog[ai].vals_off = nvals;
            if (nc_px_engine_program_compile(src[e].asset, &out->aprog[ai].prog) != ESP_OK) {
                out->skipped++;
                out->skip_prog++;
                continue;
            }
            nvals += out->aprog[ai].nv;
            naprog++;
        }

        // 展開 group → tmp；有 range 就在 tmp 上取子序列，再放入 pool
        uint32_t n = group_fill(src[e].group, inv, ninv, tmp, total_need);
        if (n == 0) {
            out->skipped++;
            out->skip_empty++;
            continue;
        }
        if (src[e].has_range) {
            const uint32_t m = slice_fill(src[e].start, src[e].stop, src[e].step, n,
                                          pool + pool_used, total_need - pool_used);
            if (m == 0) {
                out->skipped++;
                out->skip_empty++;
                continue;
            }
            for (uint32_t k = 0; k < m; k++) {
                pool[pool_used + k] = tmp[pool[pool_used + k]];
            }
            n = m;
        } else {
            if (pool_used + n > total_need) {
                out->skipped++;
                out->skip_oom++;
                continue;
            }
            memcpy(pool + pool_used, tmp, (size_t)n * sizeof(uint32_t));
        }
        src[e].off = pool_used;
        src[e].n = n;
        pool_used += n;

        out->job[kept].aprog = (uint16_t)ai;
        out->job[kept].write = src[e].write;
        out->job[kept].nled = n;
        // 用 kept 取代 e：把 src[e] 的資料搬去 src[kept]
        if (kept != e) {
            src[kept] = src[e];
        }
        for (uint32_t k = 0; k < n; k++) {
            const uint32_t d = pool[src[kept].off + k];
            if (d < px) {
                counts[d]++;
            }
        }
        kept++;
    }

    out->njobs = kept;
    if (kept == 0) {
        lfree(pool);
        lfree(tmp);
        lfree(counts);
        lfree(cursor);
        lfree(e_job);
        lfree(e_src);
        lfree(src);
        lfree(ht);
        nc_px_layout_free(out);
        return ESP_ERR_NOT_FOUND;
    }
    out->nvals = nvals;
    out->naprog = naprog;

    // ── 4. prefix sum → bucket 起點 ──
    uint32_t acc = 0;
    for (uint32_t d = 0; d < px; d++) {
        const uint32_t c = counts[d];
        counts[d] = acc;
        acc += c;
    }
    memcpy(cursor, counts, (size_t)px * sizeof(uint32_t));

    // ── 5. 填 bucket（entry 次序 = 穩定）──
    for (uint32_t e = 0; e < kept; e++) {
        for (uint32_t k = 0; k < src[e].n; k++) {
            const uint32_t d = pool[src[e].off + k];
            if (d >= px) {
                continue;
            }
            const uint32_t pos = cursor[d]++;
            e_job[pos] = (uint16_t)e;
            e_src[pos] = k;
        }
    }

    // ── 6. 掃 dst，合併連續段 ──
    uint32_t nruns = 0;
    for (uint32_t d = 0; d < px; d++) {
        const uint32_t beg = counts[d];
        const uint32_t end = cursor[d];
        for (uint32_t pos = beg; pos < end; pos++) {
            const uint32_t job = e_job[pos];
            const uint32_t sk = e_src[pos];
            nc_px_lrun_t *last = (nruns > 0) ? &out->run[nruns - 1] : NULL;
            if (last != NULL && last->job == job && last->src + last->count == sk &&
                last->dst + last->count == d) {
                last->count++;
            } else {
                out->run[nruns].dst = d;
                out->run[nruns].src = sk;
                out->run[nruns].count = 1;
                out->run[nruns].job = job;
                nruns++;
            }
        }
    }
    out->nruns = nruns;

    lfree(pool);
    lfree(tmp);
    lfree(counts);
    lfree(cursor);
    lfree(e_job);
    lfree(e_src);
    lfree(src);
    lfree(ht);
    // 6 B 單表：換場時展開一次（失敗就維持 nflat = 0，熱路徑照行 runs）
    (void)nc_px_layout_flatify(out);
    return ESP_OK;
}

void nc_px_layout_free(nc_px_layout_t *l)
{
    if (l == NULL) {
        return;
    }
    lfree(l->aprog);
    lfree(l->job);
    lfree(l->run);
    lfree(l->flat);
    memset(l, 0, sizeof(*l));
}

// ═══════════════════════ 6 B 落點表 ═══════════════════════
// runs → 每顆一筆（見 nc_px_layout.h 的 nc_px_flat6_t）。
// 展開完之後熱路徑：讀 6 B → 查值 → 寫 4 B，冇 `%`、冇 job 追蹤。
_Static_assert(sizeof(nc_px_flat6_t) == 6, "nc_px_flat6_t 一定要 6 B（dst2 src2 write1 aprog1）");

// 一個 write 每顆前進幾多個值（同 apply 的 kk／c3x／c4x 一致）
static uint32_t write_step(uint8_t write)
{
    switch (write) {
    case NC_PX_WRITE_RGB:
        return 3;
    case NC_PX_WRITE_RGBW:
        return 4;
    default:
        return 1;
    }
}

size_t nc_px_layout_flat_bytes(const nc_px_layout_t *l)
{
    return (l == NULL) ? 0 : ((size_t)l->nflat * sizeof(nc_px_flat6_t));
}

esp_err_t nc_px_layout_flatify(nc_px_layout_t *l)
{
    if (l == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    // 重新展開（build／load 之後叫；改名／重建時可以再叫一次）
    lfree(l->flat);
    l->flat = NULL;
    l->nflat = 0;
    if (l->nruns == 0 || l->px == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    // 16-bit 欄位的邊界：唔夠位就唔展開（run 路徑照做，唔會靜靜截斷）
    if (l->px > 0xFFFFu || l->naprog > 0xFFu) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (uint32_t a = 0; a < l->naprog; a++) {
        if (l->aprog[a].nv == 0 || l->aprog[a].nv > 0xFFFFu) {
            return ESP_ERR_INVALID_SIZE;   // nv == 0：run 路徑寫 0，唔可以省
        }
    }
    // 總顆數（重疊會多過 px；太多就唔展開——記憶體唔值得）
    size_t total = 0;
    for (uint32_t r = 0; r < l->nruns; r++) {
        const nc_px_ljob_t *job = &l->job[l->run[r].job];
        const uint32_t nv = l->aprog[job->aprog].nv;
        // 內圈用「加完最多減一次 nv」，所以要確認通道起點 + 步長 < 2×nv
        const uint32_t need = (job->write == NC_PX_WRITE_RGBW) ? 4u
                            : (job->write == NC_PX_WRITE_RGB) ? 3u : 1u;
        if (need > 1u && nv < need) {
            return ESP_ERR_INVALID_SIZE;
        }
        total += l->run[r].count;
        if (total > (size_t)l->px * 8u || total > (1u << 20)) {
            return ESP_ERR_INVALID_SIZE;
        }
    }
    nc_px_flat6_t *f = (nc_px_flat6_t *)ialloc(total * sizeof(nc_px_flat6_t));
    if (f == NULL) {
        return ESP_ERR_NO_MEM;
    }
    size_t k = 0;
    for (uint32_t r = 0; r < l->nruns; r++) {
        const nc_px_lrun_t *run = &l->run[r];
        const nc_px_ljob_t *job = &l->job[run->job];
        const uint32_t nv = l->aprog[job->aprog].nv;
        const uint32_t step = write_step(job->write);
        // 值流起點 = (步長 × 段內第幾顆) 減約到 nv 之內；之後每顆 +step 回繞
        uint32_t c = (step * (run->src % nv)) % nv;
        for (uint32_t x = 0; x < run->count; x++, k++) {
            f[k].dst = (uint16_t)(run->dst + x);
            f[k].src = (uint16_t)c;
            f[k].write = job->write;
            f[k].aprog = (uint8_t)job->aprog;
            c += step;
            while (c >= nv) {
                c -= nv;
            }
        }
    }
    l->flat = f;
    l->nflat = (uint32_t)k;
    return ESP_OK;
}

// 6 B 表版本的 8-bit 寫入（內圈：一次乘法都冇）
static uint32_t apply_flat(const nc_px_layout_t *l, uint16_t *const *vals, uint8_t *dst)
{
    for (uint32_t i = 0; i < l->nflat; i++) {
        const nc_px_flat6_t *e = &l->flat[i];
        const uint16_t *v = vals[e->aprog];
        const uint32_t nv = l->aprog[e->aprog].nv;
        uint8_t *o = dst + ((size_t)e->dst << 2);
        const uint32_t c = e->src;
        switch (e->write) {
        case NC_PX_WRITE_RGB: {
            const uint32_t c1 = (c + 1u < nv) ? (c + 1u) : (c + 1u - nv);
            const uint32_t c2 = (c + 2u < nv) ? (c + 2u) : (c + 2u - nv);
            // 注意：呢度**刻意**用 4 個 byte store，唔用一次 32-bit store。
            // 真機量到 32-bit store 反而慢 1.5×（2,953 → 4,393 µs，500 組）——byte store
            // 俾 store buffer 併入同一條 cache line，一個 word store 就沒了這個好處。
            o[0] = (uint8_t)(v[c] >> 4);
            o[1] = (uint8_t)(v[c1] >> 4);
            o[2] = (uint8_t)(v[c2] >> 4);
            o[3] = 0;
            break;
        }
        case NC_PX_WRITE_RGBW: {
            const uint32_t c1 = (c + 1u < nv) ? (c + 1u) : (c + 1u - nv);
            const uint32_t c2 = (c + 2u < nv) ? (c + 2u) : (c + 2u - nv);
            const uint32_t c3 = (c + 3u < nv) ? (c + 3u) : (c + 3u - nv);
            o[0] = (uint8_t)(v[c] >> 4);
            o[1] = (uint8_t)(v[c1] >> 4);
            o[2] = (uint8_t)(v[c2] >> 4);
            o[3] = (uint8_t)(v[c3] >> 4);
            break;
        }
        case NC_PX_WRITE_R:
            o[0] = (uint8_t)(v[c] >> 4);
            break;
        case NC_PX_WRITE_G:
            o[1] = (uint8_t)(v[c] >> 4);
            break;
        case NC_PX_WRITE_B:
            o[2] = (uint8_t)(v[c] >> 4);
            break;
        case NC_PX_WRITE_W:
            o[3] = (uint8_t)(v[c] >> 4);
            break;
        case NC_PX_WRITE_WW: {
            const uint16_t val = v[c];
            o[0] = 0;
            o[1] = 0;
            o[2] = (uint8_t)(val & 0xFFu);
            o[3] = (uint8_t)(val >> 8);
            break;
        }
        case NC_PX_WRITE_WWWW: {
            const uint8_t b = (uint8_t)(v[c] >> 4);
            o[0] = b;
            o[1] = b;
            o[2] = b;
            o[3] = b;
            break;
        }
        default:
            break;
        }
    }
    return l->nflat;
}

// ═══════════════════════ apply ═══════════════════════
uint32_t nc_px_layout_apply(const nc_px_layout_t *l, uint16_t *const *vals, uint8_t *dst)
{
    if (l == NULL || vals == NULL || dst == NULL) {
        return 0;
    }
    if (l->nflat > 0 && l->flat != NULL) {
        return apply_flat(l, vals, dst);
    }
    for (uint32_t r = 0; r < l->nruns; r++) {
        const nc_px_lrun_t *run = &l->run[r];
        const nc_px_ljob_t *job = &l->job[run->job];
        const nc_px_aprog_t *ap = &l->aprog[job->aprog];
        const uint16_t *v = vals[job->aprog];
        const uint32_t nv = ap->nv;
        // 值流 index 用「計數器回繞」代替每顆 `% nv`：rgb 9,600 顆 = 28,800 次除法，
        // 板上量到係 apply 的主要成本（3.4 ms → 1 ms 級）。
        uint32_t c0 = (nv > 0) ? (run->src % nv) : 0;
        uint32_t c1 = (nv > 0) ? ((run->src + 1u) % nv) : 0;
        uint32_t c2 = (nv > 0) ? ((run->src + 2u) % nv) : 0;
        uint32_t c3 = (nv > 0) ? ((run->src + 3u) % nv) : 0;
        uint32_t kk = (nv > 0) ? (run->src % nv) : 0;
        uint32_t c3x = (nv > 0) ? ((3u * run->src) % nv) : 0;
        uint32_t c3y = (nv > 0) ? ((3u * run->src + 1u) % nv) : 0;
        uint32_t c3z = (nv > 0) ? ((3u * run->src + 2u) % nv) : 0;
        uint32_t c4x = (nv > 0) ? ((4u * run->src) % nv) : 0;
        uint32_t c4y = (nv > 0) ? ((4u * run->src + 1u) % nv) : 0;
        uint32_t c4z = (nv > 0) ? ((4u * run->src + 2u) % nv) : 0;
        uint32_t c4w = (nv > 0) ? ((4u * run->src + 3u) % nv) : 0;
        for (uint32_t x = 0; x < run->count; x++) {
            uint8_t *o = dst + ((size_t)(run->dst + x) << 2);
            switch (job->write) {
            case NC_PX_WRITE_R:
                o[0] = (nv > 0) ? (uint8_t)(v[kk] >> 4) : 0;
                if (++kk == nv) {
                    kk = 0;
                }
                break;
            case NC_PX_WRITE_G:
                o[1] = (nv > 0) ? (uint8_t)(v[kk] >> 4) : 0;
                if (++kk == nv) {
                    kk = 0;
                }
                break;
            case NC_PX_WRITE_B:
                o[2] = (nv > 0) ? (uint8_t)(v[kk] >> 4) : 0;
                if (++kk == nv) {
                    kk = 0;
                }
                break;
            case NC_PX_WRITE_W:
                o[3] = (nv > 0) ? (uint8_t)(v[kk] >> 4) : 0;
                if (++kk == nv) {
                    kk = 0;
                }
                break;
            case NC_PX_WRITE_WW:
                o[0] = 0;
                o[1] = 0;
                if (nv > 0) {
                    const uint16_t val = v[kk];
                    o[2] = (uint8_t)(val & 0xFFu);
                    o[3] = (uint8_t)(val >> 8);
                    if (++kk == nv) {
                        kk = 0;
                    }
                } else {
                    o[2] = 0;
                    o[3] = 0;
                }
                break;
            case NC_PX_WRITE_RGB:
                o[0] = (nv > 0) ? (uint8_t)(v[c3x] >> 4) : 0;
                o[1] = (nv > 0) ? (uint8_t)(v[c3y] >> 4) : 0;
                o[2] = (nv > 0) ? (uint8_t)(v[c3z] >> 4) : 0;
                o[3] = 0;
                if (nv > 0) {
                    // rgb 每顆前進 3 個值（不是 1）；用減法回繞避免 %
                    c3x += 3u;
                    while (c3x >= nv) {
                        c3x -= nv;
                    }
                    c3y += 3u;
                    while (c3y >= nv) {
                        c3y -= nv;
                    }
                    c3z += 3u;
                    while (c3z >= nv) {
                        c3z -= nv;
                    }
                }
                break;
            case NC_PX_WRITE_RGBW:
                o[0] = (nv > 0) ? (uint8_t)(v[c4x] >> 4) : 0;
                o[1] = (nv > 0) ? (uint8_t)(v[c4y] >> 4) : 0;
                o[2] = (nv > 0) ? (uint8_t)(v[c4z] >> 4) : 0;
                o[3] = (nv > 0) ? (uint8_t)(v[c4w] >> 4) : 0;
                if (nv > 0) {
                    // rgbw 每顆前進 4 個值
                    c4x += 4u;
                    while (c4x >= nv) {
                        c4x -= nv;
                    }
                    c4y += 4u;
                    while (c4y >= nv) {
                        c4y -= nv;
                    }
                    c4z += 4u;
                    while (c4z >= nv) {
                        c4z -= nv;
                    }
                    c4w += 4u;
                    while (c4w >= nv) {
                        c4w -= nv;
                    }
                }
                break;
            case NC_PX_WRITE_WWWW: {
                const uint8_t b = (nv > 0) ? (uint8_t)(v[kk] >> 4) : 0;
                o[0] = b;
                o[1] = b;
                o[2] = b;
                o[3] = b;
                if (nv > 0) {
                    if (++kk == nv) {
                        kk = 0;
                    }
                }
                break;
            }
            default:
                if (nv > 0) {
                    if (++kk == nv) {
                        kk = 0;
                    }
                }
                break;
            }
        }
        (void)c0;
        (void)c1;
        (void)c2;
        (void)c3;
    }
    return l->nruns;
}

// 6 B 表版本的抖動子幀寫入（rgb／rgbw 才做；其他 write 同 run 路徑一樣跳過）
static void apply_dither_flat(const nc_px_layout_t *l, uint16_t *const vals[],
                              uint8_t *dst, uint8_t subframes, uint8_t pos)
{
    for (uint32_t i = 0; i < l->nflat; i++) {
        const nc_px_flat6_t *e = &l->flat[i];
        if (e->write != NC_PX_WRITE_RGB && e->write != NC_PX_WRITE_RGBW) {
            continue;
        }
        const uint16_t *v = vals[e->aprog];
        const uint32_t nv = l->aprog[e->aprog].nv;
        const uint32_t nch = (e->write == NC_PX_WRITE_RGBW) ? 4u : 3u;
        uint8_t *o = dst + ((size_t)e->dst << 2);
        const uint32_t c = e->src;
        for (uint32_t ch = 0; ch < nch; ch++) {
            uint32_t cc = c + ch;
            if (cc >= nv) {
                cc -= nv;                 // nv ≥ nch 已由 flatify 檢查，減一次夠
            }
            const uint32_t v12 = v[cc];
            const uint32_t base = v12 >> 4;
            const uint32_t n1 = (((v12 & 15u) * subframes) + 8u) >> 4;
            o[ch] = (uint8_t)(base + ((base < 255u && pos < n1) ? 1u : 0u));
        }
        if (nch == 3u) {
            o[3] = 0;
        }
    }
}

// 只寫一個子幀：環 API 一次只可以 begin 一格，所以 production 用這個逐格寫。
void nc_px_layout_apply_dither(const nc_px_layout_t *l, uint16_t *const vals[],
                               uint8_t *dst, uint8_t subframes, uint8_t sf, uint32_t phase)
{
    if (l == NULL || vals == NULL || dst == NULL || subframes == 0) {
        return;
    }
    const uint8_t pos = (uint8_t)((sf + phase) % subframes);
    if (l->nflat > 0 && l->flat != NULL) {
        apply_dither_flat(l, vals, dst, subframes, pos);
        return;
    }
    for (uint32_t r = 0; r < l->nruns; r++) {
        const nc_px_lrun_t *run = &l->run[r];
        const nc_px_ljob_t *job = &l->job[run->job];
        const nc_px_aprog_t *ap = &l->aprog[job->aprog];
        const uint16_t *v = vals[job->aprog];
        const uint32_t nv = ap->nv;
        if (job->write != NC_PX_WRITE_RGB && job->write != NC_PX_WRITE_RGBW) {
            continue;
        }
        const uint32_t nch = (job->write == NC_PX_WRITE_RGBW) ? 4u : 3u;
        uint32_t c[4] = { 0, 0, 0, 0 };
        if (nv > 0) {
            for (uint32_t ch = 0; ch < nch; ch++) {
                c[ch] = (nch * run->src + ch) % nv;
            }
        }
        for (uint32_t x = 0; x < run->count; x++) {
            const size_t o = (size_t)(run->dst + x) << 2;
            for (uint32_t ch = 0; ch < nch; ch++) {
                const uint32_t v12 = (nv > 0) ? v[c[ch]] : 0u;
                const uint32_t base = v12 >> 4;
                const uint32_t n1 = (((v12 & 15u) * subframes) + 8u) >> 4;
                dst[o + ch] = (uint8_t)(base + ((base < 255u && pos < n1) ? 1u : 0u));
            }
            if (nch == 3) {
                dst[o + 3] = 0;
            }
            if (nv > 0) {
                for (uint32_t ch = 0; ch < nch; ch++) {
                    c[ch] += nch;
                    while (c[ch] >= nv) {
                        c[ch] -= nv;
                    }
                }
            }
        }
    }
}

// ═══════════════════════ apply4（抖動）═══════════════════════
void nc_px_layout_apply4(const nc_px_layout_t *l, uint16_t *const vals[],
                         uint8_t *const dst[], uint8_t subframes, uint32_t phase)
{
    if (l == NULL || vals == NULL || dst == NULL || subframes == 0) {
        return;
    }
    for (uint32_t r = 0; r < l->nruns; r++) {
        const nc_px_lrun_t *run = &l->run[r];
        const nc_px_ljob_t *job = &l->job[run->job];
        const nc_px_aprog_t *ap = &l->aprog[job->aprog];
        const uint16_t *v = vals[job->aprog];
        const uint32_t nv = ap->nv;
        if (job->write != NC_PX_WRITE_RGB && job->write != NC_PX_WRITE_RGBW) {
            continue;                     // 其他寫入型別唔做抖動（保持原位）
        }
        const uint32_t nch = (job->write == NC_PX_WRITE_RGBW) ? 4u : 3u;
        uint32_t c[4] = { 0, 0, 0, 0 };
        if (nv > 0) {
            for (uint32_t ch = 0; ch < nch; ch++) {
                c[ch] = (nch * run->src + ch) % nv;
            }
        }
        for (uint32_t x = 0; x < run->count; x++) {
            const size_t o = (size_t)(run->dst + x) << 2;
            for (uint32_t ch = 0; ch < nch; ch++) {
                const uint32_t v12 = (nv > 0) ? v[c[ch]] : 0u;
                const uint32_t base = v12 >> 4;
                const uint32_t n1 = (((v12 & 15u) * subframes) + 8u) >> 4;
                for (uint8_t sf = 0; sf < subframes; sf++) {
                    const uint8_t pos = (uint8_t)((sf + phase) % subframes);
                    dst[sf][o + ch] = (uint8_t)(base + ((base < 255u && pos < n1) ? 1u : 0u));
                }
            }
            if (nch == 3) {
                for (uint8_t sf = 0; sf < subframes; sf++) {
                    dst[sf][o + 3] = 0;
                }
            }
            if (nv > 0) {
                for (uint32_t ch = 0; ch < nch; ch++) {
                    c[ch] += nch;
                    while (c[ch] >= nv) {
                        c[ch] -= nv;
                    }
                }
            }
        }
    }
}

uint32_t nc_px_layout_apply12(const nc_px_layout_t *l, uint16_t *const *vals, uint16_t *dst)
{
    if (l == NULL || vals == NULL || dst == NULL) {
        return 0;
    }
    for (uint32_t r = 0; r < l->nruns; r++) {
        const nc_px_lrun_t *run = &l->run[r];
        const nc_px_ljob_t *job = &l->job[run->job];
        const nc_px_aprog_t *ap = &l->aprog[job->aprog];
        const uint16_t *v = vals[job->aprog];
        const uint32_t nv = ap->nv;
        uint32_t kk = (nv > 0) ? (run->src % nv) : 0;
        uint32_t c3x = (nv > 0) ? ((3u * run->src) % nv) : 0;
        uint32_t c3y = (nv > 0) ? ((3u * run->src + 1u) % nv) : 0;
        uint32_t c3z = (nv > 0) ? ((3u * run->src + 2u) % nv) : 0;
        uint32_t c4x = (nv > 0) ? ((4u * run->src) % nv) : 0;
        uint32_t c4y = (nv > 0) ? ((4u * run->src + 1u) % nv) : 0;
        uint32_t c4z = (nv > 0) ? ((4u * run->src + 2u) % nv) : 0;
        uint32_t c4w = (nv > 0) ? ((4u * run->src + 3u) % nv) : 0;
        for (uint32_t x = 0; x < run->count; x++) {
            uint16_t *o = dst + ((size_t)(run->dst + x) << 2);
            switch (job->write) {
            case NC_PX_WRITE_R:
                o[0] = (nv > 0) ? v[kk] : 0;
                if (++kk == nv) {
                    kk = 0;
                }
                break;
            case NC_PX_WRITE_G:
                o[1] = (nv > 0) ? v[kk] : 0;
                if (++kk == nv) {
                    kk = 0;
                }
                break;
            case NC_PX_WRITE_B:
                o[2] = (nv > 0) ? v[kk] : 0;
                if (++kk == nv) {
                    kk = 0;
                }
                break;
            case NC_PX_WRITE_W:
                o[3] = (nv > 0) ? v[kk] : 0;
                if (++kk == nv) {
                    kk = 0;
                }
                break;
            case NC_PX_WRITE_WW:
                o[0] = 0;
                o[1] = 0;
                if (nv > 0) {
                    const uint16_t val = v[kk];
                    o[2] = (uint16_t)(val & 0xFFu);
                    o[3] = (uint16_t)(val >> 8);
                    if (++kk == nv) {
                        kk = 0;
                    }
                } else {
                    o[2] = 0;
                    o[3] = 0;
                }
                break;
            case NC_PX_WRITE_RGB:
                o[0] = (nv > 0) ? v[c3x] : 0;
                o[1] = (nv > 0) ? v[c3y] : 0;
                o[2] = (nv > 0) ? v[c3z] : 0;
                o[3] = 0;
                if (nv > 0) {
                    // rgb 每顆前進 3 個值（不是 1）；用減法回繞避免 %
                    c3x += 3u;
                    while (c3x >= nv) {
                        c3x -= nv;
                    }
                    c3y += 3u;
                    while (c3y >= nv) {
                        c3y -= nv;
                    }
                    c3z += 3u;
                    while (c3z >= nv) {
                        c3z -= nv;
                    }
                }
                break;
            case NC_PX_WRITE_RGBW:
                o[0] = (nv > 0) ? v[c4x] : 0;
                o[1] = (nv > 0) ? v[c4y] : 0;
                o[2] = (nv > 0) ? v[c4z] : 0;
                o[3] = (nv > 0) ? v[c4w] : 0;
                if (nv > 0) {
                    // rgbw 每顆前進 4 個值
                    c4x += 4u;
                    while (c4x >= nv) {
                        c4x -= nv;
                    }
                    c4y += 4u;
                    while (c4y >= nv) {
                        c4y -= nv;
                    }
                    c4z += 4u;
                    while (c4z >= nv) {
                        c4z -= nv;
                    }
                    c4w += 4u;
                    while (c4w >= nv) {
                        c4w -= nv;
                    }
                }
                break;
            case NC_PX_WRITE_WWWW: {
                const uint16_t b = (nv > 0) ? v[kk] : 0;
                o[0] = b;
                o[1] = b;
                o[2] = b;
                o[3] = b;
                if (nv > 0) {
                    if (++kk == nv) {
                        kk = 0;
                    }
                }
                break;
            }
            default:
                if (nv > 0) {
                    if (++kk == nv) {
                        kk = 0;
                    }
                }
                break;
            }
        }
    }
    return l->nruns;
}

uint32_t nc_px_aprog_vals(const nc_px_aprog_t *ap, uint32_t step, uint16_t *out, uint32_t cap)
{
    if (ap == NULL || ap->asset == NULL || out == NULL || cap == 0) {
        return 0;
    }
    const nc_px_asset_t *a = ap->asset;
    uint32_t n = ap->nv;
    if (n > cap) {
        n = cap;
    }
    const uint32_t speed = (a->speed == 0) ? 1 : a->speed;
    uint32_t ph = ((step / speed) * (uint32_t)a->step + (uint32_t)a->offset);
    ph = (ap->prog.total != 0) ? (ph % ap->prog.total) : 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t idx = a->reverse ? (n - 1u - i) : i;
        uint32_t p = ph + idx * (uint32_t)a->spacing;
        p = (ap->prog.total != 0) ? (p % ap->prog.total) : 0;
        out[i] = nc_px_engine_program_value(&ap->prog, p);
    }
    return n;
}

// ═══════════════════════ 永久儲存（快取）═══════════════════════
// 為什麼：組合建立要 39–137 ms（視段數），而且每個組合都唔一樣；
// 唔可以靠「攤分」去當佢免費。所以編譯一次就寫成檔案，下次直接讀（固定時間）。
// 格式：header（magic/ver/key/各表長度/crc）＋ aprog* ＋ job* ＋ run*。
typedef struct {
    uint32_t magic;
    uint32_t ver;
    uint32_t key;
    uint32_t px;
    uint32_t naprog;
    uint32_t njobs;
    uint32_t nruns;
    uint32_t nvals;
    uint32_t payload_bytes;
    uint32_t crc;
} pxlt_hdr_t;

#define PXLT_MAGIC 0x544C5850u   // "PXLT"

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

uint32_t nc_px_layout_cache_key(uint32_t mode_hash, uint32_t map_hash, uint32_t px)
{
    uint32_t h = 2166136261u;
    const uint32_t v[4] = { mode_hash, map_hash, px, NC_PX_LAYOUT_CACHE_VER };
    for (int i = 0; i < 4; i++) {
        uint8_t *b = (uint8_t *)&v[i];
        for (int k = 0; k < 4; k++) {
            h ^= b[k];
            h *= 16777619u;
        }
    }
    return h;
}

// 一條 aprog 的存檔形狀（asset 用名字，program 原樣）
typedef struct {
    char     name[NC_PX_NAME_MAX];
    uint32_t nv;
    uint32_t vals_off;
    nc_px_prog_t prog;
} pxlt_aprog_t;

typedef struct {
    uint32_t aprog;
    uint32_t write;
    uint32_t nled;
} pxlt_job_t;

size_t nc_px_layout_cache_bytes(const nc_px_layout_t *l)
{
    if (l == NULL) {
        return 0;
    }
    return (size_t)l->naprog * sizeof(pxlt_aprog_t) + (size_t)l->njobs * sizeof(pxlt_job_t) +
           (size_t)l->nruns * sizeof(nc_px_lrun_t);
}

esp_err_t nc_px_layout_save(const nc_px_layout_t *l, const char *path, uint32_t key)
{
    if (l == NULL || path == NULL || l->px == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t pa = (size_t)l->naprog * sizeof(pxlt_aprog_t);
    const size_t pj = (size_t)l->njobs * sizeof(pxlt_job_t);
    const size_t pr = (size_t)l->nruns * sizeof(nc_px_lrun_t);
    const size_t payload = pa + pj + pr;
    uint8_t *buf = (uint8_t *)lalloc(payload ? payload : 1);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    size_t off = 0;
    for (uint32_t i = 0; i < l->naprog; i++) {
        pxlt_aprog_t a;
        memset(&a, 0, sizeof(a));
        snprintf(a.name, sizeof(a.name), "%s", l->aprog[i].asset ? l->aprog[i].asset->name : "");
        a.nv = l->aprog[i].nv;
        a.vals_off = l->aprog[i].vals_off;
        a.prog = l->aprog[i].prog;
        memcpy(buf + off, &a, sizeof(a));
        off += sizeof(a);
    }
    for (uint32_t i = 0; i < l->njobs; i++) {
        pxlt_job_t j;
        j.aprog = l->job[i].aprog;
        j.write = l->job[i].write;
        j.nled = l->job[i].nled;
        memcpy(buf + off, &j, sizeof(j));
        off += sizeof(j);
    }
    for (uint32_t i = 0; i < l->nruns; i++) {
        memcpy(buf + off, &l->run[i], sizeof(nc_px_lrun_t));
        off += sizeof(nc_px_lrun_t);
    }
    pxlt_hdr_t h;
    memset(&h, 0, sizeof(h));
    h.magic = PXLT_MAGIC;
    h.ver = NC_PX_LAYOUT_CACHE_VER;
    h.key = key;
    h.px = l->px;
    h.naprog = l->naprog;
    h.njobs = l->njobs;
    h.nruns = l->nruns;
    h.nvals = l->nvals;
    h.payload_bytes = (uint32_t)payload;
    h.crc = crc32_bytes(buf, payload);
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        lfree(buf);
        return ESP_ERR_NOT_FOUND;
    }
    const bool ok = fwrite(&h, sizeof(h), 1, f) == 1 && fwrite(buf, payload, 1, f) == 1;
    fclose(f);
    lfree(buf);
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t nc_px_layout_load(nc_px_layout_t *out, const char *path, uint32_t key,
                            const nc_px_asset_tbl_t *assets)
{
    if (out == NULL || path == NULL || assets == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    pxlt_hdr_t h;
    if (fread(&h, sizeof(h), 1, f) != 1 || h.magic != PXLT_MAGIC ||
        h.ver != NC_PX_LAYOUT_CACHE_VER || h.key != key || h.payload_bytes == 0 ||
        h.payload_bytes > (1u << 20)) {
        fclose(f);
        return ESP_ERR_INVALID_STATE;     // 版本／組合／損壞 → 由呼叫端重建
    }
    uint8_t *buf = (uint8_t *)lalloc(h.payload_bytes);
    if (buf == NULL) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    const size_t got = fread(buf, 1, h.payload_bytes, f);
    fclose(f);
    if (got != h.payload_bytes || crc32_bytes(buf, h.payload_bytes) != h.crc) {
        lfree(buf);
        return ESP_ERR_INVALID_STATE;
    }
    const size_t pa = (size_t)h.naprog * sizeof(pxlt_aprog_t);
    const size_t pj = (size_t)h.njobs * sizeof(pxlt_job_t);
    const size_t pr = (size_t)h.nruns * sizeof(nc_px_lrun_t);
    if (pa + pj + pr != h.payload_bytes) {
        lfree(buf);
        return ESP_ERR_INVALID_STATE;
    }
    out->px = h.px;
    out->naprog = h.naprog;
    out->njobs = h.njobs;
    out->nruns = h.nruns;
    out->nvals = h.nvals;
    out->aprog = (nc_px_aprog_t *)lalloc((size_t)h.naprog * sizeof(nc_px_aprog_t));
    out->job = (nc_px_ljob_t *)lalloc((size_t)h.njobs * sizeof(nc_px_ljob_t));
    out->run = (nc_px_lrun_t *)lalloc((size_t)h.nruns * sizeof(nc_px_lrun_t));
    if ((h.naprog && out->aprog == NULL) || (h.njobs && out->job == NULL) ||
        (h.nruns && out->run == NULL)) {
        nc_px_layout_free(out);
        lfree(buf);
        return ESP_ERR_NO_MEM;
    }
    size_t off = 0;
    for (uint32_t i = 0; i < h.naprog; i++) {
        pxlt_aprog_t a;
        memcpy(&a, buf + off, sizeof(a));
        off += sizeof(a);
        const nc_px_asset_t *asset = NULL;
        for (uint16_t k = 0; k < assets->n; k++) {
            if (strcmp(assets->item[k].name, a.name) == 0) {
                asset = &assets->item[k];
                break;
            }
        }
        if (asset == NULL) {
            nc_px_layout_free(out);
            lfree(buf);
            return ESP_ERR_INVALID_STATE;   // 內容改過（效果唔存在）→ 重建
        }
        out->aprog[i].asset = asset;
        out->aprog[i].nv = a.nv;
        out->aprog[i].vals_off = a.vals_off;
        out->aprog[i].prog = a.prog;
    }
    for (uint32_t i = 0; i < h.njobs; i++) {
        pxlt_job_t j;
        memcpy(&j, buf + off, sizeof(j));
        off += sizeof(j);
        out->job[i].aprog = (uint16_t)j.aprog;
        out->job[i].write = (uint8_t)j.write;
        out->job[i].nled = j.nled;
    }
    for (uint32_t i = 0; i < h.nruns; i++) {
        memcpy(&out->run[i], buf + off, sizeof(nc_px_lrun_t));
        off += sizeof(nc_px_lrun_t);
    }
    lfree(buf);
    // 表唔入快取檔（衍生資料）：讀完一樣展開，行為同 build 一模一樣
    (void)nc_px_layout_flatify(out);
    return ESP_OK;
}
