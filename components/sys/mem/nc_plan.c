#include "nc_plan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef NC_PLAN_HOST_TEST
#include "plan_host.h"
#define PLAN_LOG(...) plan_host_log(__VA_ARGS__)
static void *plan_alloc(size_t bytes, size_t align, nc_plan_class_t cls)
{
    return plan_host_alloc(bytes, align, (int)cls);
}
#else
#include "esp_heap_caps.h"
#include "esp_log.h"
static const char *TAG = "plan";
#define PLAN_LOG(fmt, ...) ESP_LOGI(TAG, fmt, ##__VA_ARGS__)

static void *plan_alloc(size_t bytes, size_t align, nc_plan_class_t cls)
{
    uint32_t caps = MALLOC_CAP_8BIT;
    switch (cls) {
    case NC_PLAN_DMA:
        caps |= MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL;
        break;
    case NC_PLAN_PSRAM:
        caps |= MALLOC_CAP_SPIRAM;
        break;
    case NC_PLAN_INTERNAL:
    default:
        caps |= MALLOC_CAP_INTERNAL;
        break;
    }
    if (align < 32) {
        align = 32;
    }
    return heap_caps_aligned_alloc(align, bytes, caps);
}
#endif

typedef struct {
    nc_plan_decl_t decl;
    void *ptr;
    bool ok;
    size_t offset;
} plan_entry_t;

static plan_entry_t s_entries[NC_PLAN_MAX_DECLS];
static int s_count;
static bool s_committed;
static nc_plan_report_t s_report;
static void *s_carve[NC_PLAN_CLASSES];
static int s_order[NC_PLAN_MAX_DECLS];

void nc_plan_reset(void)
{
    memset(s_entries, 0, sizeof(s_entries));
    memset(&s_report, 0, sizeof(s_report));
    memset(s_carve, 0, sizeof(s_carve));
    s_count = 0;
    s_committed = false;
}

esp_err_t nc_plan_declare(const nc_plan_decl_t *decl)
{
    if (decl == NULL || decl->name == NULL || decl->bytes == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_committed) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_count >= NC_PLAN_MAX_DECLS) {
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_entries[i].decl.name, decl->name) == 0) {
            return ESP_ERR_INVALID_STATE;
        }
    }

    s_entries[s_count].decl = *decl;
    size_t align = decl->align ? decl->align : 32;
    s_entries[s_count].decl.align = (align + 31u) & ~(size_t)31u;

    s_report.declared[decl->cls] += decl->bytes;
    s_report.decl_count = ++s_count;
    return ESP_OK;
}

static void build_order(void)
{
    for (int i = 0; i < s_count; i++) {
        s_order[i] = i;
    }
    for (int i = 1; i < s_count; i++) {
        int key = s_order[i];
        int j = i - 1;
        while (j >= 0 && s_entries[s_order[j]].decl.level > s_entries[key].decl.level) {
            s_order[j + 1] = s_order[j];
            j--;
        }
        s_order[j + 1] = key;
    }
}

esp_err_t nc_plan_commit(const size_t avail[NC_PLAN_CLASSES], bool allow_shed)
{
    (void)allow_shed;   // v2: 不再整批成敗；配不到就標記該服務失敗
    if (avail == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_committed) {
        return ESP_ERR_INVALID_STATE;
    }
    memcpy(s_report.avail, avail, sizeof(s_report.avail));
    build_order();

    size_t used[NC_PLAN_CLASSES] = {0};
    for (int k = 0; k < s_count; k++) {
        plan_entry_t *e = &s_entries[s_order[k]];
        const size_t need = e->decl.bytes;
        if (used[e->decl.cls] + need <= avail[e->decl.cls]) {
            e->offset = used[e->decl.cls];
            used[e->decl.cls] += need;
            e->ok = true;
            s_report.planned[e->decl.cls] += need;
            s_report.ok_count++;
        } else {
            e->ok = false;
            s_report.fail_count++;
            PLAN_LOG("plan: %s/%s 失敗（要 %u，剩 %u）",
                     e->decl.service ? e->decl.service : "-", e->decl.name,
                     (unsigned)need,
                     (unsigned)(avail[e->decl.cls] > used[e->decl.cls]
                                    ? avail[e->decl.cls] - used[e->decl.cls] : 0));
        }
    }

    for (int c = 0; c < NC_PLAN_CLASSES; c++) {
        if (s_report.planned[c] == 0) {
            continue;
        }
        s_carve[c] = plan_alloc(s_report.planned[c], 32, (nc_plan_class_t)c);
        if (s_carve[c] == NULL) {
            PLAN_LOG("plan: class %d carve %u B 失敗", c, (unsigned)s_report.planned[c]);
            for (int i = 0; i < s_count; i++) {
                if (s_entries[i].ok && (int)s_entries[i].decl.cls == c) {
                    s_entries[i].ok = false;
                    s_report.ok_count--;
                    s_report.fail_count++;
                }
            }
            s_report.planned[c] = 0;
            continue;
        }
        s_report.allocated[c] = s_report.planned[c];
    }

    for (int i = 0; i < s_count; i++) {
        plan_entry_t *e = &s_entries[i];
        if (e->ok) {
            e->ptr = (uint8_t *)s_carve[e->decl.cls] + e->offset;
        }
    }

    s_report.committed = true;
    s_committed = true;
    PLAN_LOG("plan: %d 條宣告、失敗 %d、DMA=%u INT=%u PSRAM=%u（每 class 一次 carve）",
             s_report.decl_count, s_report.fail_count,
             (unsigned)s_report.planned[NC_PLAN_DMA],
             (unsigned)s_report.planned[NC_PLAN_INTERNAL],
             (unsigned)s_report.planned[NC_PLAN_PSRAM]);
    return ESP_OK;
}

void *nc_plan_get(const char *name)
{
    if (name == NULL || !s_committed) {
        return NULL;
    }
    for (int i = 0; i < s_count; i++) {
        if (s_entries[i].ok && strcmp(s_entries[i].decl.name, name) == 0) {
            return s_entries[i].ptr;
        }
    }
    return NULL;
}

const nc_plan_report_t *nc_plan_report(void)
{
    return &s_report;
}

bool nc_plan_was_shed(const char *name)
{
    if (name == NULL) {
        return false;
    }
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_entries[i].decl.name, name) == 0) {
            return !s_entries[i].ok;
        }
    }
    return false;
}

size_t nc_plan_json(char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) {
        return 0;
    }
    size_t off = 0;
    off += snprintf(buf + off, cap - off,
                    "{\"ok\":%s,\"decl\":%d,\"fail\":%d,"
                    "\"bytes\":{\"dma\":%u,\"int\":%u,\"psram\":%u},\"items\":[",
                    s_report.fail_count == 0 ? "true" : "false",
                    s_report.decl_count, s_report.fail_count,
                    (unsigned)s_report.planned[NC_PLAN_DMA],
                    (unsigned)s_report.planned[NC_PLAN_INTERNAL],
                    (unsigned)s_report.planned[NC_PLAN_PSRAM]);
    bool first = true;
    for (int i = 0; i < s_count && off < cap; i++) {
        const plan_entry_t *e = &s_entries[i];
        off += snprintf(buf + off, cap - off,
                        "%s{\"svc\":\"%s\",\"n\":\"%s\",\"lvl\":%d,\"b\":%u,\"ok\":%s}",
                        first ? "" : ",",
                        e->decl.service ? e->decl.service : "",
                        e->decl.name, e->decl.level,
                        (unsigned)e->decl.bytes,
                        e->ok ? "true" : "false");
        first = false;
    }
    if (off < cap) {
        off += snprintf(buf + off, cap - off, "]}");
    }
    return off;
}
