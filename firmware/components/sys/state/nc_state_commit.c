// S3：把 RAM store 回寫成 state.ncst（用户明確要求 persist 時）。
//
// 命名：唔可以叫 nc_state_commit()——nc_state.c 有一個 NVS 世代的同名函式，
// 兩者同時存在會揀錯（2026-11-27 實測：寫入回 OK 但 blob 冇變）。
//
// 為什麼 src_hash 要沿用 boot 算的那個：
//   下次開機用「cfg 造出的 records」重算 hash；如果 commit 用「改過的值」算，
//   hash 必然唔同 → 開機當成 config 改過 → 重建 → 回寫值被清掉。
//   沿用 boot 的 hash，就係「呢份 cache 仍然對應同一份 config」的意思。
#include "nc_state_boot.h"

#include <string.h>

#include "esp_heap_caps.h"

#include "nc_state_blob.h"
#include "nc_state_file.h"
#include "nc_state_store.h"

static bool s_persist_pending;   // decode 任務只排隊；main 喺 reboot 前落盤

void nc_state_persist_request(void)
{
    s_persist_pending = true;
}

bool nc_state_persist_pending(void)
{
    return s_persist_pending;
}

esp_err_t nc_state_persist_path(const char *path)
{
    const nc_state_store_t *st = nc_state_boot_store();
    if (path == NULL || st == NULL || st->n == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    // 唔可以放 stack：STATUS_SET 喺 decode 任務（3,072 B stack）執行，
    // 96 × 24 B = 2,304 B 會直接爆（2026-11-27 實測 rst=4 panic）。
    nc_state_rec_t *recs = (nc_state_rec_t *)heap_caps_malloc(
        sizeof(nc_state_rec_t) * st->n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (recs == NULL) {
        return ESP_ERR_NO_MEM;
    }
    for (uint16_t i = 0; i < st->n; i++) {
        const nc_state_ent_t *e = &st->e[i];
        recs[i] = (nc_state_rec_t){ .key = e->key, .type = e->type };
        if (e->type == NC_STATE_T_SINT) {
            recs[i].i = e->i;
        } else {
            recs[i].u = e->u;
        }
    }
    const nc_state_meta_t meta = {
        .ver = NC_STATE_BLOB_VER,
        .flags = 0,
        .src_hash = nc_state_boot_src_hash(),
    };
    const esp_err_t se = nc_state_file_save(path, &meta, recs, st->n);
    heap_caps_free(recs);
    return se;
}

esp_err_t nc_state_persist(void)
{
    const esp_err_t se = nc_state_persist_path(NC_STATE_PATH);
    if (se == ESP_OK) {
        s_persist_pending = false;
    }
    return se;
}
