#include "nc_action.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "action";

static nc_action_t *s_actions;
static size_t s_count;

static int cmp_cmd(const void *a, const void *b)
{
    const nc_action_t *x = (const nc_action_t *)a;
    const nc_action_t *y = (const nc_action_t *)b;
    return (x->cmd < y->cmd) ? -1 : (x->cmd > y->cmd) ? 1 : 0;
}

esp_err_t nc_action_init(const nc_action_table_t *const *tables, size_t n_tables)
{
    if (tables == NULL || n_tables == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_actions != NULL) {
        return ESP_ERR_INVALID_STATE;    // 只可以初始化一次
    }

    // ① 統計：先把總數算清楚，才知道要配置幾多
    size_t total = 0;
    for (size_t t = 0; t < n_tables; t++) {
        if (tables[t] == NULL || tables[t]->list == NULL) {
            continue;
        }
        total += tables[t]->count;
    }
    if (total == 0) {
        ESP_LOGW(TAG, "冇任何指令");
        return ESP_OK;
    }

    // ② 一次過配置剛好大小（不是倍增、不是固定上限 → 不會碎片）
    nc_action_t *tab = (nc_action_t *)malloc(total * sizeof(nc_action_t));
    if (tab == NULL) {
        ESP_LOGE(TAG, "指令表配置失敗（%u × %u B）", (unsigned)total,
                 (unsigned)sizeof(nc_action_t));
        return ESP_ERR_NO_MEM;
    }

    // ③ 填入（**先到先得**：重複的 cmd 在填入時就擋掉，後面的表蓋不掉前面的）
    //
    // 為什麼要在這裡擋、而不是排序後再合併：qsort 對相等 key 的次序**沒有保證**，
    // 所以「保留前者」在排序後其實是「保留某一個」。實測（2026-11-08）：同一個映像裡
    // 0xF012 保留了 test 的 BIG_REPLY、0xF013 卻保留了 core 的 SD_BENCH_RESULT ——
    // 相等 key 誰勝出是由排序結果決定，換一次 build 就可能反過來。
    size_t n = 0;
    for (size_t t = 0; t < n_tables; t++) {
        const nc_action_table_t *tb = tables[t];
        if (tb == NULL || tb->list == NULL) {
            continue;
        }
        for (size_t i = 0; i < tb->count; i++) {
            if (tb->list[i].fn == NULL) {
                ESP_LOGW(TAG, "[%s] 0x%04X 冇 handler，跳過", tb->svc, (unsigned)tb->list[i].cmd);
                continue;
            }
            if (n >= total) {                 // 正常唔會發生（total 就係各表 count 之和）
                ESP_LOGE(TAG, "[%s] 統計與填入不一致 → 停", tb->svc);
                break;
            }
            bool dup = false;
            for (size_t k = 0; k < n; k++) {
                if (tab[k].cmd == tb->list[i].cmd) {
                    ESP_LOGE(TAG, "指令 0x%04X 重複：[%s]（後來）與 [%s]（已登記）→ 丟棄後來者",
                             (unsigned)tb->list[i].cmd,
                             tb->list[i].owner ? tb->list[i].owner : "-",
                             tab[k].owner ? tab[k].owner : "-");
                    dup = true;
                    break;
                }
            }
            if (dup) {
                continue;
            }
            tab[n++] = tb->list[i];
        }
    }

    // ④ 排序（dispatch 用二分搜尋）。重複在 ③ 已經擋掉，這裡只做保險檢查。
    qsort(tab, n, sizeof(tab[0]), cmp_cmd);
    for (size_t i = 1; i < n; i++) {
        if (tab[i].cmd == tab[i - 1].cmd) {
            ESP_LOGE(TAG, "指令 0x%04X 仍然重複（不應該發生）", (unsigned)tab[i].cmd);
        }
    }

    s_actions = tab;
    s_count = n;
    ESP_LOGI(TAG, "指令表：%u 條、%u B（一次配置、已封存）",
             (unsigned)s_count, (unsigned)(s_count * sizeof(nc_action_t)));
    return ESP_OK;
}

size_t nc_action_count(void)
{
    return s_count;
}

size_t nc_action_bytes(void)
{
    return s_count * sizeof(nc_action_t);
}

const nc_action_t *nc_action_all(void)
{
    return s_actions;
}

bool nc_action_at(size_t i, uint16_t *cmd, const char **owner, nc4_handler_t *fn)
{
    if (s_actions == NULL || i >= s_count) {
        return false;
    }
    if (cmd != NULL) {
        *cmd = s_actions[i].cmd;
    }
    if (owner != NULL) {
        *owner = s_actions[i].owner;
    }
    if (fn != NULL) {
        *fn = s_actions[i].fn;
    }
    return true;
}

nc4_handler_t nc_action_find(uint16_t cmd)
{
    if (s_actions == NULL) {
        return NULL;
    }
    size_t lo = 0, hi = s_count;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (s_actions[mid].cmd == cmd) {
            return s_actions[mid].fn;
        }
        if (s_actions[mid].cmd < cmd) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return NULL;
}
