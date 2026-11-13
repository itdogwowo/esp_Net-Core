// 任務管理器（registry）：登記 → 宣告資源 → 啟動 → 開 loop → 量測 → 狀態。
//
// 這是 mp_Net-Core task_manager.py 的 C 版：
//   ① register：只登記（不啟動），依 level 排序
//   ② declare：mem[] + declare() → nc_plan（同時跑帳本：重名 / 指令 / 接線）
//   ③ start  ：資源到手 → on_start() → 註冊指令 → 準備 loop（還不開）
//   ④ boot   ：依 layer 開 loop（0 先）
//
// 帳本規則（與 mp 版 TaskManager 的防呆同精神）：
//   · 同一條 cmd 只能有一個主人（後者不算）
//   · exclusive 的接線 id 只能有一個主人
//   · 同一個 provide 名字只能有一個主人
//   · core 不設限：跑哪一核是宣告的一部分，框架不改寫（硬體互斥用 claims[] 表達）
#include "nc_task_priv.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nc_cfg.h"
#include "cJSON.h"
#include "nc_buf.h"
#include "nc_core.h"

static const char *TAG = "tm";

static size_t jappend(char *buf, size_t cap, size_t off, const char *fmt, ...);

// ── 任務 slot ──
typedef struct {
    bool used;
    bool started;              // on_start 已跑、指令已註冊
    bool runtime;              // attach 進來的
    const nc_task_t *def;
    char name[32];             // 穩定副本（指令/provide 的 owner 指向這裡）

    nc_task_res_t req[NC_TASK_MEM_MAX];
    nc_task_res_view_t view[NC_TASK_MEM_MAX];
    int nreq;

    int nclaims;
    uint32_t claim_off;        // bit k = 第 k 條 claim 不算（獨占衝突，audit 之後有效）
    int nprov;

    nc_task_rt_t loops[NC_TASK_LOOPS_MAX];
    int nloops;

    // tasks.json 的覆寫（開機讀一次）
    bool cfg_seen;             // config 有這個名字
    int  cfg_level;            // -1 = 不啟動
    int  cfg_aff[2];           // [core0, core1]；-1 = 沒寫
} tm_slot_t;

// ── 對外提供 ──
typedef struct {
    bool used;
    const nc_task_provide_t *p;
    char owner[32];
} prov_slot_t;

static tm_slot_t s_slots[NC_TASK_MAX];
static prov_slot_t s_provs[NC_PROVIDE_MAX];
static int s_count;

// ───────────────────────── 名字表 ─────────────────────────

static const char *const s_kind_names[NC_TASK_KIND_COUNT] = {
    "system", "normal", "effect",
};

const char *nc_task_kind_name(nc_task_kind_t kind)
{
    if (kind < 0 || kind >= NC_TASK_KIND_COUNT) {
        return "?";
    }
    return s_kind_names[kind];
}

bool nc_task_kind_parse(const char *s, nc_task_kind_t *out)
{
    if (s == NULL || out == NULL) {
        return false;
    }
    // "service" / "task" 都是 NORMAL 的舊說法 —— task 與 service 是同一件事
    if (strcmp(s, "service") == 0 || strcmp(s, "task") == 0 || strcmp(s, "normal") == 0) {
        *out = NC_TASK_NORMAL;
        return true;
    }
    for (int i = 0; i < NC_TASK_KIND_COUNT; i++) {
        if (strcmp(s, s_kind_names[i]) == 0) {
            *out = (nc_task_kind_t)i;
            return true;
        }
    }
    return false;
}

bool nc_task_kind_is_runtime(nc_task_kind_t kind)
{
    return kind >= 0 && kind < NC_TASK_KIND_COUNT && kind != NC_TASK_SYSTEM;
}

// ───────────────────────── 對外提供 ─────────────────────────

const char *nc_provide_kind_name(nc_provide_kind_t kind)
{
    switch (kind) {
    case NC_PROVIDE_BUF:  return "buf";
    case NC_PROVIDE_JSON: return "json";
    case NC_PROVIDE_U32:  return "u32";
    default:              return "?";
    }
}

static const prov_slot_t *prov_slot_find(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (int i = 0; i < NC_PROVIDE_MAX; i++) {
        if (s_provs[i].used && strcmp(s_provs[i].p->name, name) == 0) {
            return &s_provs[i];
        }
    }
    return NULL;
}

const nc_task_provide_t *nc_provide_find(const char *name)
{
    const prov_slot_t *s = prov_slot_find(name);
    return s ? s->p : NULL;
}

const char *nc_provide_owner(const char *name)
{
    const prov_slot_t *s = prov_slot_find(name);
    return s ? s->owner : NULL;
}

bool nc_provide_buf(const char *name, void **ptr, size_t *bytes)
{
    const nc_task_provide_t *p = nc_provide_find(name);
    if (p == NULL || p->kind != NC_PROVIDE_BUF || p->buf == NULL) {
        return false;
    }
    void *b = NULL;
    size_t n = 0;
    if (!p->buf(&b, &n) || b == NULL) {
        return false;
    }
    if (ptr != NULL) {
        *ptr = b;
    }
    if (bytes != NULL) {
        *bytes = n;
    }
    return true;
}

size_t nc_provide_json(const char *name, char *out, size_t cap)
{
    const nc_task_provide_t *p = nc_provide_find(name);
    if (p == NULL || p->kind != NC_PROVIDE_JSON || p->json == NULL ||
        out == NULL || cap == 0) {
        return 0;
    }
    return p->json(out, cap);
}

bool nc_provide_u32(const char *name, uint32_t *out)
{
    const nc_task_provide_t *p = nc_provide_find(name);
    if (p == NULL || p->kind != NC_PROVIDE_U32 || p->u32 == NULL) {
        return false;
    }
    return p->u32(out);
}

int nc_provide_count(void)
{
    int n = 0;
    for (int i = 0; i < NC_PROVIDE_MAX; i++) {
        if (s_provs[i].used) {
            n++;
        }
    }
    return n;
}

size_t nc_provide_list_json(char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) {
        return 0;
    }
    size_t off = jappend(buf, cap, 0, "[");
    bool first = true;
    for (int i = 0; i < NC_PROVIDE_MAX && off < cap - 1; i++) {
        if (!s_provs[i].used) {
            continue;
        }
        const nc_task_provide_t *p = s_provs[i].p;
        off = jappend(buf, cap, off,
                      "%s{\"name\":\"%s\",\"owner\":\"%s\",\"kind\":\"%s\",\"desc\":\"%s\"",
                      first ? "" : ",", p->name, s_provs[i].owner,
                      nc_provide_kind_name(p->kind), p->desc ? p->desc : "");
        first = false;
        if (p->kind == NC_PROVIDE_BUF && p->buf != NULL) {
            void *b = NULL;
            size_t n = 0;
            if (p->buf(&b, &n) && b != NULL) {
                off = jappend(buf, cap, off, ",\"bytes\":%u", (unsigned)n);
            }
        }
        off = jappend(buf, cap, off, "}");
    }
    return jappend(buf, cap, off, "]");
}

static void prov_register(tm_slot_t *s)
{
    const nc_task_t *t = s->def;
    s->nprov = 0;
    if (t->provides == NULL) {
        return;
    }
    for (int i = 0; i < t->nprovides && i < NC_TASK_PROV_MAX; i++) {
        const nc_task_provide_t *p = &t->provides[i];
        if (p->name == NULL) {
            continue;
        }
        if (nc_provide_find(p->name) != NULL) {
            ESP_LOGE(TAG, "CONFLICT provide %s：已被 %s 佔用 → %s 不登記",
                     p->name, nc_provide_owner(p->name), s->name);
            continue;
        }
        bool placed = false;
        for (int k = 0; k < NC_PROVIDE_MAX; k++) {
            if (!s_provs[k].used) {
                s_provs[k].used = true;
                s_provs[k].p = p;
                snprintf(s_provs[k].owner, sizeof(s_provs[k].owner), "%s", s->name);
                s->nprov++;
                placed = true;
                break;
            }
        }
        if (!placed) {
            ESP_LOGE(TAG, "provide 表滿了（%d）→ %s 沒登記", NC_PROVIDE_MAX, p->name);
            break;
        }
    }
}

static void prov_unregister(const char *owner)
{
    if (owner == NULL) {
        return;
    }
    for (int i = 0; i < NC_PROVIDE_MAX; i++) {
        if (s_provs[i].used && strcmp(s_provs[i].owner, owner) == 0) {
            memset(&s_provs[i], 0, sizeof(s_provs[i]));
        }
    }
}

// ───────────────────────── slot 表 ─────────────────────────

static tm_slot_t *find_slot(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (int i = 0; i < s_count; i++) {
        if (s_slots[i].used && strcmp(s_slots[i].name, name) == 0) {
            return &s_slots[i];
        }
    }
    return NULL;
}

void nc_task_reset(void)
{
    memset(s_slots, 0, sizeof(s_slots));
    memset(s_provs, 0, sizeof(s_provs));
    s_count = 0;
}

static esp_err_t insert_slot(const nc_task_t *t, bool runtime)
{
    if (t == NULL || t->name == NULL || strlen(t->name) >= sizeof(s_slots[0].name)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (t->kind < 0 || t->kind >= NC_TASK_KIND_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (find_slot(t->name) != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_count >= NC_TASK_MAX) {
        return ESP_ERR_NO_MEM;
    }

    // 依 level 插入（小的先）；同 level 保持登記順序
    int pos = s_count;
    while (pos > 0 && s_slots[pos - 1].def->level > t->level) {
        s_slots[pos] = s_slots[pos - 1];
        pos--;
    }
    memset(&s_slots[pos], 0, sizeof(s_slots[pos]));
    s_slots[pos].used = true;
    s_slots[pos].runtime = runtime;
    s_slots[pos].def = t;
    snprintf(s_slots[pos].name, sizeof(s_slots[pos].name), "%s", t->name);
    s_count++;

    ESP_LOGI(TAG, "register [%s] kind=%s level=%d%s", t->name,
             nc_task_kind_name(t->kind), t->level, runtime ? "（runtime）" : "");
    return ESP_OK;
}

esp_err_t nc_task_register(const nc_task_t *task)
{
    return insert_slot(task, false);
}

// ───────────────────────── 宣告 ─────────────────────────

static int declare_one(tm_slot_t *s)
{
    const nc_task_t *t = s->def;
    int n = 0;

    if (t->mem != NULL) {
        for (int i = 0; i < t->nmem && n < NC_TASK_MEM_MAX; i++) {
            s->req[n++] = t->mem[i];
        }
    }
    if (t->declare != NULL) {
        const int extra = t->declare(&s->req[n], NC_TASK_MEM_MAX - n);
        if (extra > 0) {
            n += (extra > NC_TASK_MEM_MAX - n) ? (NC_TASK_MEM_MAX - n) : extra;
        }
    }
    s->nreq = n;

    int declared = 0;
    for (int i = 0; i < n; i++) {
        const nc_task_res_t *r = &s->req[i];
        const esp_err_t e = nc_plan_declare(&(nc_plan_decl_t){
            .service = s->name, .name = r->name, .level = t->level,
            .cls = r->cls, .bytes = r->bytes, .align = r->align,
        });
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "[%s] 宣告 %s 失敗 %s（重名？）", s->name, r->name,
                     esp_err_to_name(e));
        } else {
            declared++;
        }
    }

    s->nclaims = (t->claims != NULL) ? (t->nclaims > NC_TASK_CLAIM_MAX
                                            ? NC_TASK_CLAIM_MAX : t->nclaims) : 0;
    s->claim_off = 0;
    return declared;
}

// 帳本：重複的指令、衝突的接線。壞的一律「後者不算」。
//
// 以前的寫法在迴圈裡直接 `nclaims--`：外層 k 會跳過一條，而 nclaims 縮短之後
// 尾段根本沒檢查到 —— 所以第二個衝突永遠不會被發現。而且「後者不算」應該是
// 「這條 claim 不算」，不是「任務少一個 claim」。現在只記錄，不改 nclaims。
static void audit(void)
{
    int conflicts = 0;
    for (int i = 0; i < s_count; i++) {
        const nc_task_t *ti = s_slots[i].def;
        s_slots[i].claim_off = 0;
        const int nk = s_slots[i].nclaims;
        if (ti->claims == NULL || nk <= 0) {
            continue;
        }
        for (int k = 0; k < nk; k++) {
            const nc_task_claim_t *ci = &ti->claims[k];
            if (!ci->exclusive || ci->id == NULL) {
                continue;
            }
            bool hit = false;
            for (int j = 0; j < i && !hit; j++) {
                const nc_task_t *tj = s_slots[j].def;
                if (tj->claims == NULL) {
                    continue;
                }
                for (int q = 0; q < s_slots[j].nclaims; q++) {
                    // 對方自己也被人擋掉的那條，不構成衝突
                    if (s_slots[j].claim_off & (1u << q)) {
                        continue;
                    }
                    const nc_task_claim_t *cj = &tj->claims[q];
                    if (cj->exclusive && cj->id != NULL &&
                        strcmp(ci->id, cj->id) == 0) {
                        ESP_LOGE(TAG, "CONFLICT %s：%s 與 %s 都要獨占 → %s 這條 claim 不算",
                                 ci->id, s_slots[j].name, s_slots[i].name, s_slots[i].name);
                        hit = true;
                        break;
                    }
                }
            }
            if (hit) {
                s_slots[i].claim_off |= (1u << k);
                conflicts++;
            }
        }
    }
    if (conflicts > 0) {
        ESP_LOGE(TAG, "claim 衝突共 %d 條（上面逐條列出）", conflicts);
    }
}

// 真正生效的 claim 數（被擋掉的不算）
static int active_claims(const tm_slot_t *s)
{
    int n = 0;
    for (int k = 0; k < s->nclaims; k++) {
        if (!(s->claim_off & (1u << k))) {
            n++;
        }
    }
    return n;
}

// 讀 tasks 層：只取 level / affinity。名字對不上 → 用固件預設（cfg_seen = false）。
// 型別化之後這裡只讀 struct；不再自己 parse JSON，也不再有「config 讀不到」的分支。
static void load_cfg(void)
{
    const nc_tasks_cfg_t *cfg = nc_cfg_tasks();

    for (int i = 0; i < s_count; i++) {
        tm_slot_t *s = &s_slots[i];
        s->cfg_seen = false;
        s->cfg_level = s->def->level;
        // 租戶：用戶未設定 = (0,0) = 唔跑。核心：-1 = 唔受 config 管（照 C 宣告）。
        s->cfg_aff[0] = s->cfg_aff[1] = (s->def->kind == NC_TASK_SYSTEM) ? -1 : 0;

        // 核心（房東）不受租約約束：SYSTEM 任務永遠照 C 宣告跑，config 管不到它。
        if (s->def->kind == NC_TASK_SYSTEM) {
            continue;
        }
        if (cfg == NULL) {
            continue;
        }
        for (uint8_t k = 0; k < cfg->n_tasks; k++) {
            const nc_task_cfg_t *t = &cfg->tasks[k];
            if (strcmp(t->name, s->name) != 0) {
                continue;
            }
            s->cfg_seen = true;
            // config 冇寫 level → 用固件宣告（has_level 就是為此存在；
            // 直接當 0 會把「漏寫」變成「強制 level 0」，行為會靜靜改變）。
            s->cfg_level = t->has_level ? t->level : s->def->level;
            // 未寫的核一律當 0：未設定 = 唔准跑嗰核（同舊行為）。
            s->cfg_aff[0] = t->aff[0] ? 1 : 0;
            s->cfg_aff[1] = t->aff[1] ? 1 : 0;
            break;
        }
    }

    for (int i = 0; i < s_count; i++) {
        tm_slot_t *s = &s_slots[i];
        ESP_LOGI(TAG, "cfg %-10s %s level=%2d affinity=(%d,%d)%s",
                 s->name, s->def->kind == NC_TASK_SYSTEM ? "core  " : "task  ",
                 s->cfg_level, s->cfg_aff[0], s->cfg_aff[1],
                 s->cfg_seen ? " config" : "");
    }
}

// 固件有、config 沒有 → 補一條進 tasks.json（只加不改）。
// 只做租戶（normal / effect）；核心（system）永遠唔寫入 config。
// 補寫的條目一律 (0,0)：新任務預設關閉，要跑就自己改 tasks.json。
// （與「未設定 = (0,0)」一致，所以補寫前後行為一樣，唔會「重開機先跑」。）
static void append_missing_to_cfg(void)
{
    for (int i = 0; i < s_count; i++) {
        tm_slot_t *s = &s_slots[i];
        if (!s->used || s->cfg_seen || s->def->kind == NC_TASK_SYSTEM) {
            continue;
        }
        char obj[160];
        snprintf(obj, sizeof(obj),
                 "{ \"name\": \"%s\", \"level\": %d, \"affinity\": [0, 0] }",
                 s->name, s->cfg_level);
        if (nc_cfg_append_obj("tasks", "tasks", obj)) {
            s->cfg_seen = true;          // 補過就唔好再補
            ESP_LOGI(TAG, "cfg 補寫 %s", s->name);
        }
    }
}

// runtime 任務（掃 .fxe 之後 attach 的）也要補進 config。
int nc_task_sync_cfg(void)
{
    append_missing_to_cfg();
    return 0;
}

int nc_task_declare_all(void)
{
    load_cfg();
    append_missing_to_cfg();
    int total = 0;
    for (int i = 0; i < s_count; i++) {
        if (s_slots[i].used && s_slots[i].cfg_level >= 0) {
            total += declare_one(&s_slots[i]);
        }
    }
    audit();
    ESP_LOGI(TAG, "宣告完成：%d 個任務、%d 項記憶體", s_count, total);
    return total;
}

// ───────────────────────── 啟動 / 停止 ─────────────────────────

static bool gather_resources(tm_slot_t *s)
{
    for (int i = 0; i < s->nreq; i++) {
        nc_task_res_view_t *v = &s->view[i];
        v->ptr = nc_plan_get(s->req[i].name);
        v->len = s->req[i].bytes;
        v->buf = NULL;
        if (v->ptr == NULL) {
            ESP_LOGE(TAG, "[%s] 資源 %s（%u B）拿不到 → 這個任務不啟動",
                     s->name, s->req[i].name, (unsigned)s->req[i].bytes);
            return false;
        }
        // P5（2026-11-10）：宣告了「格 × 格數」就由管理器建立緩衝區，
        // on_start 直接收 nc_buf_t *（契約 C1／C3 的目標形狀）。
        if (s->req[i].slot_bytes > 0 && s->req[i].n > 0) {
            nc_buf_where_t where = NC_BUF_SRAM;
            if (s->req[i].cls == NC_PLAN_PSRAM) {
                where = NC_BUF_PSRAM;
            } else if (s->req[i].cls == NC_PLAN_DMA) {
                where = NC_BUF_DMA;
            }
            v->buf = nc_buf_at(s->req[i].name, v->ptr, v->len, where,
                               s->req[i].slot_bytes, s->req[i].n, s->req[i].flags);
            if (v->buf == NULL) {
                ESP_LOGE(TAG, "[%s] 資源 %s 建 buffer 失敗（%u × %u）→ 這個任務不啟動",
                         s->name, s->req[i].name, (unsigned)s->req[i].slot_bytes,
                         (unsigned)s->req[i].n);
                return false;
            }
        }
    }
    return true;
}

static void prepare_loops(tm_slot_t *s)
{
    const nc_task_t *t = s->def;
    s->nloops = 0;
    if (t->loops == NULL) {
        return;
    }
    for (int i = 0; i < t->nloops && i < NC_TASK_LOOPS_MAX; i++) {
        const nc_task_loop_t *d = &t->loops[i];
        if (d->name == NULL || d->fn == NULL) {
            continue;
        }
        // layer 超出範圍 = 永遠不會被 nc_task_boot() 撿到（它只掃 0..LAYERS-1）。
        // 以前係靜靜咁唔啟動，查半天；現在直接報錯，並且不建立這個 loop。
        if (d->layer < 0 || d->layer >= NC_TASK_LAYERS) {
            ESP_LOGE(TAG, "[%s/%s] layer=%d 超出 0..%d → 這個 loop 永遠不會啟動",
                     s->name, d->name, d->layer, NC_TASK_LAYERS - 1);
            continue;
        }
        nc_task_rt_t *rt = &s->loops[s->nloops++];
        memset(rt, 0, sizeof(*rt));
        rt->def = d;
        rt->kind = (uint8_t)s->def->kind;   // 優先級治理用
        rt->enabled = true;
        // 完全照宣告，但核數才是硬體事實：藍圖／config 都只是「要求」。
        // 收斂在這裡（而不是在每個 xTaskCreatePinnedToCore），單核板上寫 core1
        // 只會得到一行警告，唔會在 FreeRTOS 裡 assert → reboot loop。
        rt->core = nc_core_clamp(d->core);
        if (s->cfg_aff[0] >= 0 || s->cfg_aff[1] >= 0) {
            // 未寫的那一格當 0（不是 1）：null / 缺欄位 = 唔准跑嗰核。
            const int a0 = (s->cfg_aff[0] < 0) ? 0 : (s->cfg_aff[0] != 0);
            const int a1 = (s->cfg_aff[1] < 0) ? 0 : (s->cfg_aff[1] != 0);
            if ((a0 + a1) == 0) {
                rt->enabled = false;             // (0,0) → 唔跑（on_start 照跑，只係 loop 唔開）
            } else if (a0 && a1) {
                ESP_LOGW(TAG, "[%s] affinity=(1,1) 唔合法（一個任務只跑一核）→ 照宣告 core %d",
                         s->name, rt->core);
            } else if (!((rt->core == 0 && a0) || (rt->core == 1 && a1))) {
                // 宣告的核唔准 → 搬去准嗰個（tasks.json 寫 [0,1] 而本機單核時，
                // 這裡會收斂到 core0，而不是把 core1 交給 FreeRTOS）
                rt->core = nc_core_clamp(a0 ? 0 : 1);
            }
        }
        // 固件自己話事嘅開關（房東用）。放喺 affinity 之後：租約管唔到呢個，
        // 因為 SYSTEM 任務根本冇讀過 tasks.json（見 load_cfg）。
        if (rt->enabled && d->enabled != NULL && !d->enabled()) {
            rt->enabled = false;
        }
    }
}

static esp_err_t start_slot(tm_slot_t *s, bool start_now)
{
    if (s->started) {
        return ESP_OK;
    }
    if (!gather_resources(s)) {
        return ESP_ERR_NO_MEM;
    }

    if (s->def->on_start != NULL) {
        const esp_err_t e = s->def->on_start(s->view, s->nreq);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "[%s] on_start 失敗 %s", s->name, esp_err_to_name(e));
            return e;
        }
    }

    prepare_loops(s);
    prov_register(s);

    s->started = true;
    ESP_LOGI(TAG, "start [%s] kind=%s mem=%d loops=%d prov=%d", s->name,
             nc_task_kind_name(s->def->kind), s->nreq, s->nloops, s->nprov);

    if (start_now) {
        for (int i = 0; i < s->nloops; i++) {
            nc_task_rt_start(&s->loops[i]);
        }
    }
    return ESP_OK;
}

int nc_task_start_all(void)
{
    int started = 0;
    for (int i = 0; i < s_count; i++) {
        if (s_slots[i].used && s_slots[i].cfg_level >= 0 &&
            start_slot(&s_slots[i], false) == ESP_OK) {
            started++;
        }
    }
    ESP_LOGI(TAG, "啟動完成：%d/%d 個任務", started, s_count);
    return started;
}

int nc_task_boot(void)
{
    int started = 0;
    for (int layer = 0; layer < NC_TASK_LAYERS; layer++) {
        for (int i = 0; i < s_count; i++) {
            tm_slot_t *s = &s_slots[i];
            if (!s->used || !s->started || s->cfg_level < 0) {
                continue;
            }
            for (int k = 0; k < s->nloops; k++) {
                nc_task_rt_t *rt = &s->loops[k];
                if (rt->def->layer != layer || rt->started || !rt->enabled) {
                    continue;
                }
                if (nc_task_rt_start(rt) == ESP_OK) {
                    started++;
                }
            }
        }
    }
    ESP_LOGI(TAG, "loop 啟動：%d 個", started);
    return started;
}

static bool stop_loops(tm_slot_t *s, uint32_t wait_ms)
{
    bool all = true;
    for (int i = 0; i < s->nloops; i++) {
        if (s->loops[i].started &&
            nc_task_rt_stop(&s->loops[i], wait_ms) != ESP_OK) {
            ESP_LOGE(TAG, "[%s] loop %s 停不下來 → 不卸載（避免 use-after-free）",
                     s->name, s->loops[i].def->name ? s->loops[i].def->name : "?");
            all = false;
        }
    }
    return all;
}

// 生命結束：停 loop → on_stop → 收回指令 / provide。回 false = 停不下來。
static bool shutdown_slot(tm_slot_t *s)
{
    if (!s->started) {
        return true;
    }
    if (!stop_loops(s, 1000)) {
        return false;
    }
    if (s->def->on_stop != NULL) {
        s->def->on_stop();
    }
    prov_unregister(s->name);
    s->started = false;
    memset(s->view, 0, sizeof(s->view));
    return true;
}

void nc_task_stop_all(void)
{
    for (int i = s_count - 1; i >= 0; i--) {
        if (s_slots[i].used) {
            shutdown_slot(&s_slots[i]);
        }
    }
}

// ───────────────────────── runtime（效果任務）─────────────────────────

esp_err_t nc_task_attach(const nc_task_t *task)
{
    if (task == NULL || !nc_task_kind_is_runtime(task->kind)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (task->mem != NULL && task->nmem > 0) {
        // plan 的宣告窗口在 commit 時就關了；runtime 任務的記憶體要來自父層 arena。
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t e = insert_slot(task, true);
    if (e != ESP_OK) {
        return e;
    }
    tm_slot_t *s = find_slot(task->name);

    s->nclaims = (task->claims != NULL) ? task->nclaims : 0;

    const esp_err_t se = start_slot(s, true);
    if (se != ESP_OK) {
        nc_task_detach(task->name);
        return se;
    }
    return ESP_OK;
}

esp_err_t nc_task_detach(const char *name)
{
    tm_slot_t *s = find_slot(name);
    if (s == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!s->runtime) {
        return ESP_ERR_INVALID_STATE;      // system / normal 不可卸載
    }
    if (!shutdown_slot(s)) {
        return ESP_ERR_TIMEOUT;            // 停不下來就不要拆（指標還活著）
    }

    ESP_LOGI(TAG, "detach [%s]", s->name);
    for (int i = 0; i < s_count; i++) {
        if (&s_slots[i] == s) {
            memmove(&s_slots[i], &s_slots[i + 1], (s_count - i - 1) * sizeof(s_slots[0]));
            s_count--;
            memset(&s_slots[s_count], 0, sizeof(s_slots[0]));
            break;
        }
    }
    return ESP_OK;
}

// ───────────────────────── 單一任務控制 ─────────────────────────

esp_err_t nc_task_start(const char *name)
{
    tm_slot_t *s = find_slot(name);
    if (s == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!s->started) {
        return start_slot(s, true);
    }
    for (int i = 0; i < s->nloops; i++) {
        nc_task_rt_start(&s->loops[i]);
    }
    return ESP_OK;
}

esp_err_t nc_task_stop(const char *name, uint32_t wait_ms)
{
    tm_slot_t *s = find_slot(name);
    if (s == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    return stop_loops(s, wait_ms) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t nc_task_enable(const char *name, bool on)
{
    tm_slot_t *s = find_slot(name);
    if (s == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    for (int i = 0; i < s->nloops; i++) {
        s->loops[i].enabled = on;
    }
    return ESP_OK;
}

bool nc_task_exit_requested(void)
{
    return nc_task_rt_exit_requested();
}

// ───────────────────────── 查詢 / 狀態 ─────────────────────────

const nc_task_t *nc_task_find(const char *name)
{
    tm_slot_t *s = find_slot(name);
    return s ? s->def : NULL;
}

bool nc_task_started(const char *name)
{
    tm_slot_t *s = find_slot(name);
    return s != NULL && s->started;
}

nc_task_kind_t nc_task_kind_of(const char *name)
{
    tm_slot_t *s = find_slot(name);
    return s ? s->def->kind : NC_TASK_KIND_COUNT;
}

int nc_task_count(void)
{
    return s_count;
}

// 有界的 append：snprintf 回的是「本來想寫多少」，off 可能超過 cap。
static size_t jappend(char *buf, size_t cap, size_t off, const char *fmt, ...)
{
    if (off >= cap) {
        return cap ? cap - 1 : 0;
    }
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf + off, cap - off, fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return off;
    }
    off += (size_t)n;
    return off >= cap ? cap - 1 : off;
}

size_t nc_task_ledger(char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) {
        return 0;
    }
    size_t off = jappend(buf, cap, 0, "%-12s %-7s %-2s %-4s %s\n",
                         "task", "kind", "Lv", "mem", "loops");
    for (int i = 0; i < s_count && off < cap - 1; i++) {
        const tm_slot_t *s = &s_slots[i];
        const nc_task_t *t = s->def;
        char loops[80] = "-";
        if (s->nloops > 0) {
            size_t lo = 0;
            for (int k = 0; k < s->nloops; k++) {
                lo += (size_t)snprintf(loops + lo, sizeof(loops) - lo, "%s%s@c%d/L%d",
                                       k ? "," : "",
                                       s->loops[k].def->name ? s->loops[k].def->name : "?",
                                       s->loops[k].core, s->loops[k].def->layer);
            }
        }
        off = jappend(buf, cap, off, "%-12s %-7s %-2d %-4d %s\n",
                      s->name, nc_task_kind_name(t->kind), t->level,
                      s->nreq, loops);
    }
    return off;
}

size_t nc_task_json(char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) {
        return 0;
    }
    size_t off = jappend(buf, cap, 0, "[");
    bool first = true;
    for (int i = 0; i < s_count && off < cap - 1; i++) {
        const tm_slot_t *s = &s_slots[i];
        const nc_task_t *t = s->def;
        off = jappend(buf, cap, off,
                      "%s{\"name\":\"%s\",\"kind\":\"%s\",\"level\":%d,"
                      "\"cfg\":%d,\"cfg_level\":%d,"
                      "\"started\":%d,\"runtime\":%d,\"mem\":%d,"
                      "\"claims\":%d,\"provides\":%d,\"loops\":[",
                      first ? "" : ",", s->name, nc_task_kind_name(t->kind),
                      s->cfg_seen ? s->cfg_level : t->level,
                      s->cfg_seen ? 1 : 0, s->cfg_level,
                      s->started ? 1 : 0, s->runtime ? 1 : 0,
                      s->nreq, active_claims(s), s->nprov);
        first = false;
        for (int k = 0; k < s->nloops && off < cap - 1; k++) {
            const nc_task_rt_t *rt = &s->loops[k];
            const nc_task_loop_t *d = rt->def;
            off = jappend(buf, cap, off,
                          "%s{\"name\":\"%s\",\"core\":%d,\"layer\":%d,\"prio\":%d,"
                          "\"period_ms\":%u,\"running\":%d,\"enabled\":%d,\"b\":%u}",
                          k ? "," : "", d->name ? d->name : "?", rt->core, d->layer,
                          (int)d->prio, (unsigned)d->period_ms,
                          rt->started ? 1 : 0, rt->enabled ? 1 : 0,
                          (unsigned)rt->beats);
        }
        off = jappend(buf, cap, off, "]}");
    }
    const size_t end = jappend(buf, cap, off, "]");
    if (end >= cap - 1) {
        // 放唔落：回一個合法物件，唔好送半截 JSON
        const int n = snprintf(buf, cap, "[{\"truncated\":1,\"cap\":%u}]", (unsigned)cap);
        return (n > 0) ? (size_t)n : 0;
    }
    return end;
}

// 緊湊清單：0x1005 用。9 個任務 ≈ 540 B；就算 17 個都放得落 1 KB。
size_t nc_task_list_json(char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) {
        return 0;
    }
    size_t off = jappend(buf, cap, 0, "[");
    bool first = true;
    for (int i = 0; i < s_count && off < cap - 1; i++) {
        const tm_slot_t *s = &s_slots[i];
        const nc_task_t *t = s->def;
        const bool sys = (t->kind == NC_TASK_SYSTEM);
        off = jappend(buf, cap, off,
                      "%s{\"name\":\"%s\",\"level\":%d,\"aff\":[%d,%d],"
                      "\"started\":%d,\"loops\":%d}",
                      first ? "" : ",", s->name,
                      s->cfg_seen ? s->cfg_level : t->level,
                      sys ? -1 : s->cfg_aff[0],
                      sys ? -1 : s->cfg_aff[1],
                      s->started ? 1 : 0, s->nloops);
        first = false;
    }
    const size_t end = jappend(buf, cap, off, "]");
    if (end >= cap - 1) {
        const int n = snprintf(buf, cap, "[{\"truncated\":1,\"cap\":%u}]", (unsigned)cap);
        return (n > 0) ? (size_t)n : 0;
    }
    return end;
}
