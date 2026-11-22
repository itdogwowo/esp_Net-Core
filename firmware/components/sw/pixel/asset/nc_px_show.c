// nc_px_show.c — mode 的委派 → 可執行 plan（group→ranges、effect、write、range）＋逐幀合成。
//
// 同 mp pixel_task 的語義對齊：
//   · group 引用：`mapname.groupname`／`mapid.groupid`／`groupname`（用 mode.mapping 做預設）
//   · 群組 = 各 sel 的 slice 依次展開（sel 按硬體型別查 inventory 的 base/px）
//   · mode entry 的 range 係對「群組燈鏈」再做一次 Python slice
//   · 每幀：program interpreter 算 vals[]（spacing/reverse），再按 write 散入 4 B/px
#include "nc_px_show.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nc_px_program.h"

#define VAL_MAX 256

static const nc_px_asset_t *find_asset(const nc_px_asset_tbl_t *tbl, const char *name)
{
    if (tbl == NULL || name == NULL) {
        return NULL;
    }
    for (uint8_t i = 0; i < tbl->n; i++) {
        if (strcmp(tbl->item[i].name, name) == 0) {
            return &tbl->item[i];
        }
    }
    return NULL;
}

static bool ref_is(const char *ref, const char *name, uint16_t id)
{
    if (ref == NULL || ref[0] == '\0') {
        return false;
    }
    if (strcmp(ref, name) == 0) {
        return true;
    }
    /* 數字 id：ref 全數字才當 id */
    bool all_digit = true;
    for (const char *p = ref; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            all_digit = false;
            break;
        }
    }
    return all_digit && (uint8_t)atoi(ref) == id;
}

static const nc_px_group_t *find_group(const nc_px_map_t *map, const char *ref, char *mapref_out,
                                       size_t cap)
{
    if (map == NULL || ref == NULL) {
        return NULL;
    }
    const char *g = ref;
    const char *dot = strchr(ref, '.');
    if (dot != NULL) {
        size_t n = (size_t)(dot - ref);
        if (n + 1 >= cap) {
            return NULL;
        }
        memcpy(mapref_out, ref, n);
        mapref_out[n] = '\0';
        g = dot + 1;
        if (!ref_is(mapref_out, map->name, map->id)) {
            return NULL;                     // 引用其他 mapping：呼叫端要揀啱 map
        }
    }
    for (uint32_t i = 0; i < map->ngroups; i++) {
        if (ref_is(g, map->group[i].name, map->group[i].id)) {
            return &map->group[i];
        }
    }
    return NULL;
}

static const nc_px_hw_inv_t *find_inv(const nc_px_hw_inv_t *inv, uint8_t ninv, uint8_t hw)
{
    for (uint8_t i = 0; i < ninv; i++) {
        if (inv[i].hw == hw) {
            return &inv[i];
        }
    }
    return NULL;
}

// slice → 一至幾段 [base,count]；step>1 拆成單點（上限 cap）
static int expand_slice(const char *slice, uint16_t base, uint16_t px,
                        nc_px_job_range_t *out, int cap)
{
    int16_t s = 0, st = 0, step = 0;
    if (!nc_px_slice_parse(slice, &s, &st, &step)) {
        return -1;
    }
    if (step == 0) {
        step = 1;
    }
    int32_t start = s;
    int32_t stop = (st == 0) ? (int32_t)px : (int32_t)st;
    if (start < 0) {
        start += px;
    }
    if (stop < 0) {
        stop += px;
    }
    if (start < 0) {
        start = 0;
    }
    if (stop > px) {
        stop = px;
    }
    if (step < 0 || stop <= start) {
        return 0;
    }
    if (step == 1) {
        if (cap < 1) {
            return 0;
        }
        out[0].base = (uint16_t)(base + (uint16_t)start);
        out[0].count = (uint16_t)(stop - start);
        return 1;
    }
    int n = 0;
    for (int32_t i = start; i < stop && n < cap; i += step) {
        out[n].base = (uint16_t)(base + (uint16_t)i);
        out[n].count = 1;
        n++;
    }
    return n;
}

// 在 job 的燈鏈上再套 entry range（Python slice 語義），結果覆寫 job->rng
static void apply_range(nc_px_job_t *job, int16_t s, int16_t st, int16_t step)
{
    if (step == 0) {
        step = 1;
    }
    uint16_t total = job->nled;
    int32_t start = s;
    int32_t stop = (st == 0) ? (int32_t)total : (int32_t)st;
    if (start < 0) {
        start += total;
    }
    if (stop < 0) {
        stop += total;
    }
    if (start < 0) {
        start = 0;
    }
    if (stop > total) {
        stop = total;
    }
    if (step <= 0 || stop <= start) {
        job->nranges = 0;
        job->nled = 0;
        return;
    }
    // 把「job 燈鏈的第 x 顆」對回原本的 (base,count) 段
    nc_px_job_range_t out[NC_PX_JOB_RANGE_MAX];
    int n = 0;
    for (int32_t x = start; x < stop && n < NC_PX_JOB_RANGE_MAX; x += step) {
        // 找 x 落在哪一段
        uint16_t acc = 0;
        for (uint8_t r = 0; r < job->nranges; r++) {
            if ((uint32_t)acc + job->rng[r].count > (uint32_t)x) {
                uint16_t base = (uint16_t)(job->rng[r].base + (x - acc));
                if (n > 0 && out[n - 1].base + out[n - 1].count == base) {
                    out[n - 1].count++;      // 連續就合併
                } else {
                    out[n].base = base;
                    out[n].count = 1;
                    n++;
                }
                break;
            }
            acc = (uint16_t)(acc + job->rng[r].count);
        }
    }
    job->nranges = (uint8_t)n;
    job->nled = 0;
    for (int i = 0; i < n; i++) {
        job->nled = (uint16_t)(job->nled + out[i].count);
    }
    memset(job->rng, 0, sizeof(job->rng));
    for (int i = 0; i < n; i++) {
        job->rng[i] = out[i];
    }
}

esp_err_t nc_px_show_plan(const nc_px_mode_t *mode, const nc_px_map_t *map,
                          const nc_px_asset_tbl_t *assets,
                          const nc_px_hw_inv_t *inv, uint8_t ninv,
                          nc_px_plan_t *out)
{
    if (mode == NULL || out == NULL || assets == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    uint16_t px = 0;
    for (uint8_t i = 0; i < ninv; i++) {
        const uint16_t end = (uint16_t)(inv[i].base + inv[i].px);
        if (end > px) {
            px = end;
        }
    }
    out->px = px;

    for (uint32_t e = 0; e < mode->nentry && out->njobs < NC_PX_JOB_MAX; e++) {
        const nc_px_mode_entry_t *en = &mode->entry[e];
        const nc_px_asset_t *asset = find_asset(assets, en->effect);
        char mapref[NC_PX_LAYER_NAME_MAX];
        const nc_px_group_t *group = find_group(map, en->group, mapref, sizeof(mapref));
        if (asset == NULL || group == NULL) {
            out->skipped++;
            continue;
        }
        nc_px_job_t job;
        memset(&job, 0, sizeof(job));
        job.asset = asset;
        job.write = en->write;
        if (nc_px_engine_program_compile(asset, &job.prog) != ESP_OK) {
            out->skipped++;
            continue;
        }
        // 群組 → ranges（各 sel 依次）
        for (uint32_t s = 0; s < group->nsel && job.nranges < NC_PX_JOB_RANGE_MAX; s++) {
            const nc_px_group_sel_t *sel = &group->sel[s];
            const nc_px_hw_inv_t *slot = find_inv(inv, ninv, sel->hw);
            if (slot == NULL) {
                continue;
            }
            nc_px_job_range_t tmp[NC_PX_JOB_RANGE_MAX];
            const int n = expand_slice(sel->slice, slot->base, slot->px, tmp,
                                       NC_PX_JOB_RANGE_MAX);
            for (int i = 0; i < n && job.nranges < NC_PX_JOB_RANGE_MAX; i++) {
                job.rng[job.nranges++] = tmp[i];
                job.nled = (uint16_t)(job.nled + tmp[i].count);
            }
        }
        if (job.nranges == 0) {
            out->skipped++;
            continue;
        }
        if (en->has_range) {
            apply_range(&job, en->start, en->stop, en->step);
        }
        if (job.nranges == 0 || job.nled == 0) {
            out->skipped++;
            continue;
        }
        out->job[out->njobs++] = job;
    }
    if (out->njobs == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

uint16_t nc_px_job_vals(const nc_px_job_t *job, uint32_t step, uint16_t *out, uint16_t cap)
{
    if (job == NULL || job->asset == NULL || out == NULL || cap == 0) {
        return 0;
    }
    const nc_px_asset_t *a = job->asset;
    uint16_t n = a->pixel_hint;
    if (n == 0) {
        n = 1;
    }
    if (n > cap) {
        n = cap;
    }
    const uint16_t speed = (a->speed == 0) ? 1 : a->speed;
    uint32_t ph = ((uint32_t)(step / speed) * (uint32_t)a->step + (uint32_t)a->offset);
    ph = (job->prog.total != 0) ? (ph % job->prog.total) : 0;
    for (uint16_t i = 0; i < n; i++) {
        const uint16_t idx = a->reverse ? (uint16_t)(n - 1u - i) : i;
        uint32_t p = ph + (uint32_t)idx * (uint32_t)a->spacing;
        p = (job->prog.total != 0) ? (p % job->prog.total) : 0;
        out[i] = nc_px_engine_program_value(&job->prog, p);
    }
    return n;
}

void nc_px_pair_apply(const nc_px_job_t *job, const uint16_t *vals, uint16_t nv, uint8_t *dst)
{
    if (job == NULL || dst == NULL) {
        return;
    }
    uint16_t k = 0;
    for (uint8_t r = 0; r < job->nranges; r++) {
        for (uint16_t x = 0; x < job->rng[r].count; x++, k++) {
            uint8_t *o = dst + ((uint32_t)(job->rng[r].base + x) << 2);
            switch (job->write) {
            case NC_PX_WRITE_R:
                o[0] = (nv > 0) ? (uint8_t)(vals[k % nv] >> 4) : 0;
                break;
            case NC_PX_WRITE_G:
                o[1] = (nv > 0) ? (uint8_t)(vals[k % nv] >> 4) : 0;
                break;
            case NC_PX_WRITE_B:
                o[2] = (nv > 0) ? (uint8_t)(vals[k % nv] >> 4) : 0;
                break;
            case NC_PX_WRITE_W:
                o[3] = (nv > 0) ? (uint8_t)(vals[k % nv] >> 4) : 0;
                break;
            case NC_PX_WRITE_WW: {
                o[0] = 0;
                o[1] = 0;
                if (nv > 0) {
                    const uint16_t v = vals[k % nv];
                    o[2] = (uint8_t)(v & 0xFFu);
                    o[3] = (uint8_t)(v >> 8);
                } else {
                    o[2] = 0;
                    o[3] = 0;
                }
                break;
            }
            case NC_PX_WRITE_RGB:
                if (nv > 0) {
                    o[0] = (uint8_t)(vals[(uint16_t)((3u * k) % nv)] >> 4);
                    o[1] = (uint8_t)(vals[(uint16_t)((3u * k + 1u) % nv)] >> 4);
                    o[2] = (uint8_t)(vals[(uint16_t)((3u * k + 2u) % nv)] >> 4);
                } else {
                    o[0] = 0;
                    o[1] = 0;
                    o[2] = 0;
                }
                o[3] = 0;
                break;
            case NC_PX_WRITE_RGBW:
                if (nv > 0) {
                    o[0] = (uint8_t)(vals[(uint16_t)((4u * k) % nv)] >> 4);
                    o[1] = (uint8_t)(vals[(uint16_t)((4u * k + 1u) % nv)] >> 4);
                    o[2] = (uint8_t)(vals[(uint16_t)((4u * k + 2u) % nv)] >> 4);
                    o[3] = (uint8_t)(vals[(uint16_t)((4u * k + 3u) % nv)] >> 4);
                } else {
                    o[0] = 0;
                    o[1] = 0;
                    o[2] = 0;
                    o[3] = 0;
                }
                break;
            case NC_PX_WRITE_WWWW:
                if (nv > 0) {
                    const uint8_t v8 = (uint8_t)(vals[k % nv] >> 4);
                    o[0] = v8;
                    o[1] = v8;
                    o[2] = v8;
                    o[3] = v8;
                } else {
                    o[0] = 0;
                    o[1] = 0;
                    o[2] = 0;
                    o[3] = 0;
                }
                break;
            default:
                break;
            }
        }
    }
}

void nc_px_pair_apply12(const nc_px_job_t *job, const uint16_t *vals, uint16_t nv,
                        uint16_t *dst)
{
    if (job == NULL || dst == NULL) {
        return;
    }
    uint16_t k = 0;
    for (uint8_t r = 0; r < job->nranges; r++) {
        for (uint16_t x = 0; x < job->rng[r].count; x++, k++) {
            uint16_t *o = dst + ((uint32_t)(job->rng[r].base + x) << 2);
            switch (job->write) {
            case NC_PX_WRITE_R:
                o[0] = (nv > 0) ? vals[k % nv] : 0;
                break;
            case NC_PX_WRITE_G:
                o[1] = (nv > 0) ? vals[k % nv] : 0;
                break;
            case NC_PX_WRITE_B:
                o[2] = (nv > 0) ? vals[k % nv] : 0;
                break;
            case NC_PX_WRITE_W:
                o[3] = (nv > 0) ? vals[k % nv] : 0;
                break;
            case NC_PX_WRITE_WW:
                o[0] = 0;
                o[1] = 0;
                if (nv > 0) {
                    const uint16_t v = vals[k % nv];
                    o[2] = (uint16_t)(v & 0xFFu);
                    o[3] = (uint16_t)(v >> 8);
                } else {
                    o[2] = 0;
                    o[3] = 0;
                }
                break;
            case NC_PX_WRITE_RGB:
                if (nv > 0) {
                    o[0] = vals[(uint16_t)((3u * k) % nv)];
                    o[1] = vals[(uint16_t)((3u * k + 1u) % nv)];
                    o[2] = vals[(uint16_t)((3u * k + 2u) % nv)];
                } else {
                    o[0] = 0;
                    o[1] = 0;
                    o[2] = 0;
                }
                o[3] = 0;
                break;
            case NC_PX_WRITE_RGBW:
                if (nv > 0) {
                    o[0] = vals[(uint16_t)((4u * k) % nv)];
                    o[1] = vals[(uint16_t)((4u * k + 1u) % nv)];
                    o[2] = vals[(uint16_t)((4u * k + 2u) % nv)];
                    o[3] = vals[(uint16_t)((4u * k + 3u) % nv)];
                } else {
                    o[0] = 0;
                    o[1] = 0;
                    o[2] = 0;
                    o[3] = 0;
                }
                break;
            case NC_PX_WRITE_WWWW: {
                const uint16_t v = (nv > 0) ? vals[k % nv] : 0;
                o[0] = v;
                o[1] = v;
                o[2] = v;
                o[3] = v;
                break;
            }
            default:
                break;
            }
        }
    }
}

void nc_px_canvas12_to_8(const uint16_t *src, uint8_t *dst, uint32_t nch, bool dither,
                         int32_t err[4])
{
    if (src == NULL || dst == NULL) {
        return;
    }
    for (uint32_t i = 0; i < nch; i++) {
        const uint32_t ch = i & 3u;
        int32_t acc = (int32_t)src[i] + (dither ? err[ch] : 0);
        int32_t v8 = (acc + 8) >> 4;
        if (v8 < 0) {
            v8 = 0;
        }
        if (v8 > 255) {
            v8 = 255;
        }
        dst[i] = (uint8_t)v8;
        if (dither) {
            err[ch] = acc - (v8 << 4);        // 低 4 位帶去下一顆（1D 誤差擴散）
        }
    }
}

void nc_px_show_frame(const nc_px_plan_t *plan, uint32_t step, uint8_t *dst)
{
    if (plan == NULL || dst == NULL) {
        return;
    }
    uint16_t vals[VAL_MAX];
    for (uint8_t j = 0; j < plan->njobs; j++) {
        const uint16_t nv = nc_px_job_vals(&plan->job[j], step, vals, VAL_MAX);
        nc_px_pair_apply(&plan->job[j], vals, nv, dst);
    }
}
