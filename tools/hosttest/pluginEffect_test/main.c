// 插件工具 × 效果（.nce）的 host 測試（唔需要板子、IDF、WAMR）。
//
// 驗的是效果這條路同任務唔同的地方：
//   · 容器冇 manifest → kind 由副檔名（.nce）決定 = EFFECT；
//   · 工具收到的 kind=EFFECT（同 .nct 共用同一個工具、同一張 backend 表）；
//   · 每 tick 一次 step(frame_id)，載入器量預算；
//   · 連續 3 次超支（> NC_MOD_FRAME_BUDGET_US）→ 標 bad、停用（效果唔可以拖死輸出）；
//   · status 仍然看得到「存在但 bad」，唔會靜靜消失；卸載時 close。
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "esp_rom_crc.h"
#include "mod_host.h"
#include "nc_mod.h"
#include "nc_mod_abi.h"
#include "nc_ncd.h"
#include "nc_plugin.h"

static int s_fail;
static int s_case;

static void check_impl(int cond, const char *file, int line, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("  %s  ", cond ? "PASS" : "FAIL");
    vprintf(fmt, ap);
    if (!cond) {
        printf("   (%s:%d)", file, line);
        s_fail++;
    }
    printf("\n");
    va_end(ap);
}

#define CHECK(cond, ...) check_impl((cond), __FILE__, __LINE__, __VA_ARGS__)

static void case_begin(const char *name)
{
    s_case++;
    printf("[%d] %s\n", s_case, name);
}

// ───────────────────────── 造一個 header-only .nce ─────────────────────────
#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint16_t abi;
    uint16_t flags;
    uint32_t total_size;
    uint32_t payload_off;
    uint32_t payload_size;
    uint32_t payload_crc;
    uint32_t target_id;
    uint32_t mem_bytes;
    uint32_t px;
    uint32_t fps;
    uint32_t manifest_off;
    uint32_t manifest_size;
} ncm_hdr_t;
#pragma pack(pop)

static void write_nce(const char *path, const void *payload, size_t n, uint32_t px)
{
    ncm_hdr_t h = {
        .magic = 0x314D434Eu,
        .abi = 0,
        .flags = 0,
        .total_size = (uint32_t)(sizeof(h) + n),
        .payload_off = (uint32_t)sizeof(h),
        .payload_size = (uint32_t)n,
        .payload_crc = esp_rom_crc32_le(0, payload, (unsigned int)n),
        .mem_bytes = 65536,
        .px = px,
        .fps = 30,
    };
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        perror("fopen");
        exit(2);
    }
    fwrite(&h, 1, sizeof(h), f);
    fwrite(payload, 1, n, f);
    fclose(f);
}

// ───────────────────────── 假 backend ─────────────────────────
static nc_plugin_src_t s_src;
static int      s_opens;
static int      s_steps;
static int      s_closes;
static uint32_t s_last_frame;
static int      s_slow;                  // 1 = step 拖 17 ms（超預算）

static esp_err_t fake_init(void *arena, size_t bytes)
{
    (void)arena;
    (void)bytes;
    return ESP_OK;
}

static esp_err_t fake_open(nc_plugin_t *out, const nc_plugin_src_t *src)
{
    s_src = *src;
    s_opens++;
    *out = (void *)(uintptr_t)0x1;
    return ESP_OK;
}

static esp_err_t fake_step(nc_plugin_t plugin, uint32_t frame_id)
{
    (void)plugin;
    s_steps++;
    s_last_frame = frame_id;
    if (s_slow) {
#ifdef _WIN32
        Sleep(17);
#else
        const struct timespec ts = { 0, 17 * 1000 * 1000 };   // 17 ms > 16 ms 預算
        nanosleep(&ts, NULL);
#endif
    }
    return ESP_OK;
}

static uint8_t s_fb[64];

static esp_err_t fake_frame(nc_plugin_t plugin, const uint8_t **fb, uint32_t *bytes)
{
    (void)plugin;
    *fb = s_fb;
    *bytes = sizeof(s_fb);
    return ESP_OK;
}

static void fake_close(nc_plugin_t plugin)
{
    (void)plugin;
    s_closes++;
}

static const nc_plugin_backend_t FAKE = {
    .name = "fake",
    .init = fake_init,
    .open = fake_open,
    .step = fake_step,
    .frame = fake_frame,
    .close = fake_close,
    .deinit = NULL,
};

int main(void)
{
    char dir[200];
    const char *tmp = getenv("TMPDIR");
#ifdef _WIN32
    if (tmp == NULL) {
        tmp = getenv("TEMP");
    }
#endif
    snprintf(dir, sizeof(dir), "%.180s/pluginEffect_test_mods", tmp ? tmp : "/tmp");
#ifdef _WIN32
    for (char *p = dir; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\\';
        }
    }
    _mkdir(dir);
#else
    mkdir(dir, 0755);
#endif

    char path[256];
    snprintf(path, sizeof(path), "%s/e.nce", dir);

    static uint8_t arena[64 * 1024];

    case_begin("初始化：arena ＋ 假 backend");
    CHECK(nc_mod_init(arena, sizeof(arena)) == ESP_OK, "arena 64 KB 就緒");
    CHECK(nc_plugin_set_backend(&FAKE) == ESP_OK, "backend 裝上");

    case_begin("載入 .nce → kind 以 EFFECT 傳到插件工具");
    const char payload[] = "gradient";
    write_nce(path, payload, sizeof(payload) - 1, 64);
    CHECK(nc_mod_load(path) == ESP_OK, "nc_mod_load 成功");
    CHECK(s_opens == 1, "工具 open 一次（實際 %d）", s_opens);
    CHECK(s_src.kind == NC_PLUGIN_KIND_EFFECT, "kind=EFFECT（實際 %u）", (unsigned)s_src.kind);
    CHECK(s_src.px == 64, "px=64 由 manifest 傳到工具（實際 %u）", (unsigned)s_src.px);
    CHECK(s_src.name != NULL && strcmp(s_src.name, "e") == 0, "name=e（實際 %s）",
          s_src.name ? s_src.name : "(null)");
    CHECK(nc_mod_live() != NULL, "效果自動 select");

    case_begin("ABI v0 descriptor：fb 放喺 offset 96+4K，容量對得上");
    nc_mod_desc_t d;
    CHECK(nc_plugin_desc_build(64, 65536, &d), "64 px ＋ 64 KB → 砌得成");
    CHECK(d.px_count == 64 && d.fb_bytes == 64, "px=%u fb_bytes=%u（每值 1 B）",
          (unsigned)d.px_count, (unsigned)d.fb_bytes);
    CHECK(d.fb_off == NC_PLUGIN_FB_OFF && d.sc_off == NC_PLUGIN_SCRATCH_OFF,
          "fb_off=%u sc_off=%u", (unsigned)d.fb_off, (unsigned)d.sc_off);
    CHECK(d.sc_bytes == NC_PLUGIN_SCRATCH_BYTES, "scratch=%u B", (unsigned)d.sc_bytes);
    nc_mod_desc_t bad;
    CHECK(!nc_plugin_desc_build(4096, 4096, &bad), "fb 放唔落 4 KB 線性記憶體 → 拒絕");
    CHECK(nc_plugin_desc_build(64, 0, &bad), "mem=0 = 不限 → 照砌得成");

    case_begin("平常 tick：step(frame_id) 正常，唔會標 bad");
    nc_mod_tick();
    CHECK(s_steps == 1 && s_last_frame == 1, "tick 1 → frame=%u", (unsigned)s_last_frame);
    char buf[2048];
    nc_mod_status_json(buf, sizeof(buf));
    CHECK(strstr(buf, "\"kind\":\"effect\"") != NULL, "status 見到 kind=effect");
    CHECK(strstr(buf, "\"bad\":0") != NULL, "status bad=0");

    case_begin("連續 3 次超支 → 標 bad、停用（效果唔可以拖死輸出）");
    s_slow = 1;
    nc_mod_tick();
    nc_mod_tick();
    nc_mod_tick();
    CHECK(s_steps == 4, "三次慢 tick 都有叫到 step（實際 %d）", s_steps);
    CHECK(nc_mod_live() == NULL, "超支 3 次後 live=NULL");
    nc_mod_status_json(buf, sizeof(buf));
    CHECK(strstr(buf, "\"bad\":1") != NULL, "status bad=1（存在但被停用）");
    CHECK(strstr(buf, "\"ok\":0") != NULL, "status ok=0");

    case_begin("工具交 framebuffer（0–255，px×4）");
    nc_plugin_t h = NULL;
    const nc_plugin_src_t src2 = {
        .name = "e2", .payload = (const uint8_t *)"x", .size = 1,
        .fmt = NC_PLUGIN_FMT_WASM, .kind = NC_PLUGIN_KIND_EFFECT,
        .px = 64, .mem_bytes = 65536,
    };
    CHECK(nc_plugin_open(&h, &src2) == ESP_OK, "直接開一個插件");
    const uint8_t *fb = NULL;
    uint32_t nb = 0;
    CHECK(nc_plugin_frame(h, &fb, &nb) == ESP_OK, "frame() 成功");
    CHECK(fb != NULL && nb == sizeof(s_fb), "fb=%p bytes=%u", (const void *)fb, (unsigned)nb);
    nc_plugin_close(h);

    case_begin("卸載：工具 close 一次");
    const int closes_before = s_closes;
    CHECK(nc_mod_unload("e") == ESP_OK, "unload 成功");
    CHECK(s_closes == closes_before + 1, "close 多一次（實際 %d → %d）",
          closes_before, s_closes);

    printf("\n%s（%d 個失敗）\n", s_fail ? "FAIL" : "ALL PASS", s_fail);
    return s_fail ? 1 : 0;
}
