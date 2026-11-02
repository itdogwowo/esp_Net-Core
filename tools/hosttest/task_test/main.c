// 任務管理器的 host 測試（不需要板子、不需要 IDF）
//
// 驗的是「一個 registry 管完全部」那張表：
//   登記順序（level） / 重名 / 記憶體宣告 / 指令擁有權與衝突 /
//   attach（runtime 任務） / detach / 不可卸載的 kind / 停止 / provides
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "nc4.h"
#include "nc_task.h"

// stubs.c 提供的觀察點
int      mod_stub_plan_count(void);
const char *mod_stub_plan_name(int i);
void     mod_stub_plan_dump(void);
bool     mod_stub_loop_running(const char *name);
const char *mod_stub_decoder_owner(const nc4_decoder_t *decoder, uint16_t cmd);
size_t   mod_stub_decoder_count(const nc4_decoder_t *decoder);

static int s_fail;
static int s_case;

static void check_impl(int cond, int line, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("  %s  ", cond ? "PASS" : "FAIL");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    if (!cond) {
        printf("        (%s:%d)\n", __FILE__, line);
        s_fail++;
    }
}

#define CHECK(cond, ...) check_impl((cond), __LINE__, __VA_ARGS__)

static void case_begin(const char *name)
{
    s_case++;
    printf("[%d] %s\n", s_case, name);
}

// 找某個模組在 JSON 裡的那一段（下一個 "{" 到 "}"）
static bool json_has(const char *buf, const char *name, const char *needle)
{
    char key[64];
    snprintf(key, sizeof(key), "{\"name\":\"%s\"", name);
    const char *p = strstr(buf, key);
    if (p == NULL) {
        return false;
    }
    const char *end = strstr(p, "}]}");
    const char *n = strstr(p, needle);
    return n != NULL && (end == NULL || n < end);
}

// ── 假的模組 ──
static void loop_fn(void *arg)
{
    (void)arg;
}

// 對外提供：一塊 buffer + 一個 JSON
static char s_alpha_buf[64];

static bool alpha_buf(void **ptr, size_t *bytes)
{
    *ptr = s_alpha_buf;
    *bytes = sizeof(s_alpha_buf);
    return true;
}

static size_t alpha_info(char *out, size_t cap)
{
    return (size_t)snprintf(out, cap, "{\"v\":42}");
}

static const nc_task_provide_t s_alpha_prov[] = {
    { .name = "alpha.buf", .kind = NC_PROVIDE_BUF, .desc = "測試緩衝", .buf = alpha_buf },
    { .name = "alpha.info", .kind = NC_PROVIDE_JSON, .desc = "測試資訊", .json = alpha_info },
};

// beta 故意提供同一個名字 → 帳本要擋掉
static const nc_task_provide_t s_beta_prov[] = {
    { .name = "alpha.buf", .kind = NC_PROVIDE_BUF, .desc = "撞名", .buf = alpha_buf },
};

static const nc_task_res_t s_alpha_mem[] = {
    { .name = "alpha.buf.mem", .bytes = 256, .cls = NC_PLAN_INTERNAL, .align = 32 },
};
static const nc_task_loop_t s_alpha_loops[] = {
    { .name = "alpha_loop", .fn = loop_fn, .stack = 2048, .prio = 3,
      .core = 0, .layer = 1 },
};

static const nc_task_t s_mod_alpha = {
    .name = "alpha", .kind = NC_TASK_SYSTEM, .level = 0,
    .mem = s_alpha_mem, .nmem = 1,
    .loops = s_alpha_loops, .nloops = 1,
    .provides = s_alpha_prov,
    .nprovides = sizeof(s_alpha_prov) / sizeof(s_alpha_prov[0]),
};

static const nc_task_t s_mod_beta = {
    .name = "beta", .kind = NC_TASK_NORMAL, .level = 1,
    .provides = s_beta_prov,
    .nprovides = sizeof(s_beta_prov) / sizeof(s_beta_prov[0]),
};

static const nc_task_claim_t s_gamma_claims[] = {
    { .id = "pin:16", .exclusive = true },
};
static const nc_task_t s_mod_gamma = {
    .name = "gamma", .kind = NC_TASK_NORMAL, .level = 3,
    .claims = s_gamma_claims, .nclaims = 1,
};

static const nc_task_claim_t s_delta_claims[] = {
    { .id = "pin:16", .exclusive = true },   // 跟 gamma 撞
};
static const nc_task_t s_mod_delta = {
    .name = "delta", .kind = NC_TASK_NORMAL, .level = 3,
    .claims = s_delta_claims, .nclaims = 1,
};

// runtime 租戶（效果）
static const nc_task_provide_t s_effect_prov[] = {
    { .name = "fire26.info", .kind = NC_PROVIDE_JSON, .desc = "租戶狀態", .json = alpha_info },
};
static const nc_task_t s_mod_effect = {
    .name = "fire26", .kind = NC_TASK_EFFECT, .level = 5,
    .provides = s_effect_prov,
    .nprovides = sizeof(s_effect_prov) / sizeof(s_effect_prov[0]),
};
static const nc_task_res_t s_effect_mem[] = {
    { .name = "effect.buf", .bytes = 128, .cls = NC_PLAN_INTERNAL },
};
static const nc_task_t s_mod_effect_with_mem = {
    .name = "plasma", .kind = NC_TASK_EFFECT, .level = 5,
    .mem = s_effect_mem, .nmem = 1,
};
static const nc_task_t s_mod_system_attach = {
    .name = "sneaky", .kind = NC_TASK_SYSTEM, .level = 5,
};

int main(void)
{
    static char json[4096];

    case_begin("登記（依 level 排序，不是依 add 順序）");
    nc_task_reset();
    CHECK(nc_task_register(&s_mod_gamma) == ESP_OK, "gamma L3");
    CHECK(nc_task_register(&s_mod_alpha) == ESP_OK, "alpha L0");
    CHECK(nc_task_register(&s_mod_beta) == ESP_OK, "beta L1");
    CHECK(nc_task_register(&s_mod_delta) == ESP_OK, "delta L3");
    CHECK(nc_task_count() == 4, "4 個模組");
    CHECK(nc_task_register(&s_mod_alpha) == ESP_ERR_INVALID_STATE, "同名再登記 → INVALID_STATE");
    nc_task_json(json, sizeof(json));
    {
        const char *a = strstr(json, "\"alpha\"");
        const char *b = strstr(json, "\"beta\"");
        const char *g = strstr(json, "\"gamma\"");
        CHECK(a && b && g && a < b && b < g, "JSON 順序 = alpha(0) → beta(1) → gamma(3)");
    }

    case_begin("宣告：記憶體進 plan、指令表建立");
    const int declared = nc_task_declare_all();
    CHECK(declared == 1, "只宣告 1 塊記憶體（實際 %d）", declared);
    CHECK(mod_stub_plan_count() == 1 && strcmp(mod_stub_plan_name(0), "alpha.buf.mem") == 0,
          "plan 裡是 alpha.buf.mem");
    mod_stub_plan_dump();

    case_begin("帳本：接線獨占衝突 → 後者不算");
    nc_task_json(json, sizeof(json));
    CHECK(json_has(json, "gamma", "\"claims\":1"), "gamma 保留 pin:16");
    CHECK(json_has(json, "delta", "\"claims\":0"), "delta 的 pin:16 被擋掉");

    case_begin("啟動：資源 → start → 指令（帶擁有權）→ task 註冊");
    CHECK(nc_task_start_all() == 4, "4 個都起來");
    CHECK(!mod_stub_loop_running("alpha_loop"), "loop 還沒開（等 nc_task_boot）");
    CHECK(nc_task_boot() > 0, "nc_task_boot 依 layer 開 loop");
    CHECK(mod_stub_loop_running("alpha_loop"), "alpha_loop 開了");

    case_begin("provides：登記、查詢、撞名");
    CHECK(nc_provide_count() == 2, "全系統 2 項（實際 %d）", nc_provide_count());
    CHECK(strcmp(nc_provide_owner("alpha.buf"), "alpha") == 0, "alpha.buf 的主人是 alpha");
    {
        void *p = NULL;
        size_t n = 0;
        CHECK(nc_provide_buf("alpha.buf", &p, &n) && p == (void *)s_alpha_buf &&
              n == sizeof(s_alpha_buf), "拿到 alpha.buf（%u B）", (unsigned)n);
        char js[64] = {0};
        CHECK(nc_provide_json("alpha.info", js, sizeof(js)) > 0 && strstr(js, "42") != NULL,
              "alpha.info → %s", js);
        CHECK(!nc_provide_buf("alpha.info", &p, &n), "kind 不符 → false");
        CHECK(nc_provide_find("beta.buf") == NULL && nc_provide_find("alpha.buf") != NULL,
              "beta 的撞名 provide 沒有登記");
        nc_task_json(json, sizeof(json));
        CHECK(json_has(json, "alpha", "\"provides\":2"), "modules JSON 顯示 alpha 有 2 項");
        CHECK(json_has(json, "beta", "\"provides\":0"), "beta 的是 0（被擋）");
    }

    case_begin("runtime 租戶：attach / detach");
    CHECK(nc_task_attach(&s_mod_effect) == ESP_OK, "effect attach 成功");
    CHECK(nc_task_started("fire26"), "started");
    CHECK(nc_task_count() == 5, "表上有 5 個");
    CHECK(nc_provide_find("fire26.info") != NULL, "租戶的 provide 也登記了");
    CHECK(nc_task_detach("fire26") == ESP_OK, "detach 成功");
    CHECK(!nc_task_started("fire26"), "不再是 started");
    CHECK(nc_task_find("fire26") == NULL, "從表上消失");
    CHECK(nc_provide_find("fire26.info") == NULL, "provide 也被收回");
    CHECK(nc_task_detach("fire26") == ESP_ERR_NOT_FOUND, "再 detach → NOT_FOUND");

    case_begin("runtime 的限制：不能宣告 plan 記憶體、不能自稱 system");
    CHECK(nc_task_attach(&s_mod_effect_with_mem) == ESP_ERR_INVALID_STATE,
          "帶 mem[] 的 runtime 模組被拒（plan 窗口已關）");
    CHECK(nc_task_attach(&s_mod_system_attach) == ESP_ERR_INVALID_ARG,
          "kind=system 不能 attach");
    CHECK(nc_task_count() == 4, "失敗的 attach 不留半個");

    case_begin("不可卸載的 kind");
    CHECK(nc_task_detach("alpha") == ESP_ERR_INVALID_STATE, "system 不能 detach");
    CHECK(nc_task_detach("beta") == ESP_ERR_INVALID_STATE, "service 不能 detach");
    CHECK(nc_task_detach("nope") == ESP_ERR_NOT_FOUND, "不存在 → NOT_FOUND");

    case_begin("停止：反向、指令與 provide 收回");
    nc_task_stop_all();
    CHECK(!nc_task_started("alpha"), "alpha 已停");
    CHECK(nc_provide_count() == 0, "所有 provide 都收回了（實際 %d）", nc_provide_count());

    case_begin("kind 名稱與解析");
    CHECK(strcmp(nc_task_kind_name(NC_TASK_EFFECT), "effect") == 0, "effect");
    nc_task_kind_t k = NC_TASK_KIND_COUNT;
    CHECK(nc_task_kind_parse("task", &k) && k == NC_TASK_NORMAL, "parse task");
    CHECK(!nc_task_kind_parse("bogus", &k), "未知 kind 回 false");
    CHECK(nc_task_kind_is_runtime(NC_TASK_EFFECT), "effect 可 runtime");
    CHECK(!nc_task_kind_is_runtime(NC_TASK_SYSTEM), "system 不可 runtime");

    printf("\n%s（%d 個失敗）\n", s_fail ? "FAILED" : "ALL PASS", s_fail);
    return s_fail ? 1 : 0;
}
