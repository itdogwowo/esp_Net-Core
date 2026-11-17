#include "nc_res.h"

#include <stdio.h>
#include <string.h>

#ifndef NC_MOD_HOST_TEST
#include "nc_cfg.h"
#include "nc_task.h"
#endif

static nc_res_table_t s_table;

const char *nc_res_type_name(nc_res_type_t type)
{
    switch (type) {
    case NC_RES_BUF:     return "buf";
    case NC_RES_CMD:     return "cmd";
    case NC_RES_HW:      return "hw";
    case NC_RES_PROVIDE: return "provide";
    case NC_RES_MODULE:  return "module";
    default:             return "?";
    }
}

void nc_res_reset(void)
{
    memset(&s_table, 0, sizeof(s_table));
}

int nc_res_add_prov(const char *name, nc_res_type_t type, const char *provider,
                    uint32_t bytes, bool exclusive)
{
    if (name == NULL || name[0] == '\0') {
        return -1;
    }
    for (int i = 0; i < s_table.n_prov; i++) {
        if (strcmp(s_table.provs[i].name, name) == 0) {
            // 已經有 provider：保留第一個，第二個只補 bytes（不覆蓋 provider）。
            if (s_table.provs[i].bytes == 0) {
                s_table.provs[i].bytes = bytes;
            }
            return i;
        }
    }
    if (s_table.n_prov >= NC_RES_MAX) {
        return -1;
    }
    nc_res_prov_t *p = &s_table.provs[s_table.n_prov];
    memset(p, 0, sizeof(*p));
    snprintf(p->name, sizeof(p->name), "%s", name);
    p->type = type;
    if (provider != NULL) {
        snprintf(p->provider, sizeof(p->provider), "%s", provider);
    }
    p->bytes = bytes;
    p->exclusive = exclusive;
    return s_table.n_prov++;
}

int nc_res_add_req(const char *name, nc_res_type_t type, const char *owner,
                   uint32_t bytes, bool exclusive)
{
    if (name == NULL || name[0] == '\0' || owner == NULL || owner[0] == '\0') {
        return -1;
    }
    if (s_table.n_req >= NC_RES_MAX) {
        return -1;
    }
    nc_res_req_t *r = &s_table.reqs[s_table.n_req];
    memset(r, 0, sizeof(*r));
    snprintf(r->name, sizeof(r->name), "%s", name);
    r->type = type;
    snprintf(r->owner, sizeof(r->owner), "%s", owner);
    r->bytes = bytes;
    r->exclusive = exclusive;
    return s_table.n_req++;
}

int nc_res_build(void)
{
    nc_res_reset();
#ifndef NC_MOD_HOST_TEST
    const nc_resources_cfg_t *cfg = nc_cfg_resources();
    if (cfg != NULL) {
        for (int i = 0; i < cfg->n_res; i++) {
            const nc_res_cfg_t *r = &cfg->res[i];
            nc_res_add_prov(r->name, (nc_res_type_t)r->type, r->provider,
                            r->bytes, r->exclusive);
        }
    }
#endif
    return s_table.n_prov;
}

int nc_res_merge(void)
{
    for (int i = 0; i < s_table.n_prov; i++) {
        s_table.provs[i].present = s_table.provs[i].provider[0] != '\0';
    }
    return s_table.n_prov;
}

const nc_res_prov_t *nc_res_find_prov(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (int i = 0; i < s_table.n_prov; i++) {
        if (strcmp(s_table.provs[i].name, name) == 0) {
            return &s_table.provs[i];
        }
    }
    return NULL;
}

bool nc_res_check(const char *owner, char *why, size_t cap)
{
    if (owner == NULL) {
        return true;
    }
    if (why != NULL && cap > 0) {
        why[0] = '\0';
    }
    for (int i = 0; i < s_table.n_req; i++) {
        const nc_res_req_t *r = &s_table.reqs[i];
        if (strcmp(r->owner, owner) != 0) {
            continue;
        }
        const nc_res_prov_t *p = nc_res_find_prov(r->name);
        if (p == NULL || !p->present) {
            if (why != NULL && cap > 0) {
                snprintf(why, cap, "缺少 %s（%s）", r->name, nc_res_type_name(r->type));
            }
            return false;
        }
#ifndef NC_MOD_HOST_TEST
        if (p->provider[0] != '\0' && !nc_task_started(p->provider)) {
            if (why != NULL && cap > 0) {
                snprintf(why, cap, "%s 的 provider %s 未啟動", r->name, p->provider);
            }
            return false;
        }
#endif
        if (r->bytes > 0 && p->bytes > 0 && r->bytes > p->bytes) {
            if (why != NULL && cap > 0) {
                snprintf(why, cap, "%s 只有 %u B，要 %u", r->name,
                         (unsigned)p->bytes, (unsigned)r->bytes);
            }
            return false;
        }
        if (r->exclusive) {
            for (int k = 0; k < s_table.n_req; k++) {
                if (k == i) {
                    continue;
                }
                const nc_res_req_t *o = &s_table.reqs[k];
                if (strcmp(o->name, r->name) == 0 && strcmp(o->owner, owner) != 0) {
                    if (why != NULL && cap > 0) {
                        snprintf(why, cap, "%s 被 %s 獨占", r->name, o->owner);
                    }
                    return false;
                }
            }
        }
    }
    return true;
}

size_t nc_res_json(char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) {
        return 0;
    }
    size_t off = 0;
    off += (size_t)snprintf(buf + off, cap - off, "{\"provs\":[");
    for (int i = 0; i < s_table.n_prov && off < cap; i++) {
        const nc_res_prov_t *p = &s_table.provs[i];
        off += (size_t)snprintf(buf + off, cap - off,
                                "%s{\"name\":\"%s\",\"type\":\"%s\",\"provider\":\"%s\","
                                "\"bytes\":%u,\"exclusive\":%d,\"present\":%d}",
                                i ? "," : "", p->name, nc_res_type_name(p->type),
                                p->provider, (unsigned)p->bytes,
                                p->exclusive ? 1 : 0, p->present ? 1 : 0);
    }
    if (off < cap) {
        off += (size_t)snprintf(buf + off, cap - off, "],\"reqs\":[");
    }
    for (int i = 0; i < s_table.n_req && off < cap; i++) {
        const nc_res_req_t *r = &s_table.reqs[i];
        off += (size_t)snprintf(buf + off, cap - off,
                                "%s{\"name\":\"%s\",\"type\":\"%s\",\"owner\":\"%s\","
                                "\"bytes\":%u,\"exclusive\":%d}",
                                i ? "," : "", r->name, nc_res_type_name(r->type),
                                r->owner, (unsigned)r->bytes, r->exclusive ? 1 : 0);
    }
    if (off < cap) {
        off += (size_t)snprintf(buf + off, cap - off, "]}");
    }
    return off >= cap ? cap - 1 : off;
}
