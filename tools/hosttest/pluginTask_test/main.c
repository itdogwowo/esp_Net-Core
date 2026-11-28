// 插件工具 × 任務（.nct）的 host 測試（不需要板子、不需要 IDF、不需要 WAMR）。
//
// 驗的是「工具／載入器」這條邊界（見 docs/00_pm/gap-review.md §6.4）：
//   · 容器冇 manifest → kind 由副檔名（.nct）決定 = TASK；
//   · 載入器把 kind 經 `nc_plugin_src_t` 交去插件工具（工具本身唔識 task／pixel）；
//   · 每 tick 一次 step(frame_id)，frame_id 由載入器遞增；
//   · unload 時 close 一次；重新載入仍然開得成（arena 回收）。
//
// 用假 backend 代替 WAMR：呢支測「工具契約」，真 WAMR 的行為要靠上板。
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

// ───────────────────────── 造一個 header-only .nct ─────────────────────────
// 冇 manifest：kind 由副檔名決定（.nct → TASK），正是這支要驗的路徑。
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

static void write_nct(const char *path, const void *payload, size_t n)
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
        .px = 64,
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

// ───────────────────────── 假 backend（記錄工具傳咩入來）─────────────────────────
static nc_plugin_src_t s_src;
static int      s_opens;
static int      s_steps;
static int      s_closes;
static uint32_t s_last_frame;

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
    *out = (void *)(uintptr_t)0x1;      // 假把手（工具只負責原封傳返）
    return ESP_OK;
}

static esp_err_t fake_step(nc_plugin_t plugin, uint32_t frame_id)
{
    (void)plugin;
    s_steps++;
    s_last_frame = frame_id;
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
    snprintf(dir, sizeof(dir), "%.180s/pluginTask_test_mods", tmp ? tmp : "/tmp");
#ifdef _WIN32
    for (char *p = dir; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\\';
        }
    }
    _mkdir(dir);
#else
    mkdir(dir, 0755);                    // 存在就算
#endif

    char path[256];
    snprintf(path, sizeof(path), "%s/t.nct", dir);

    static uint8_t arena[64 * 1024];

    case_begin("初始化：arena ＋ 假 backend");
    CHECK(nc_mod_init(NULL, 0) == ESP_ERR_NO_MEM, "沒有 arena → 明確失敗");
    CHECK(nc_mod_init(arena, sizeof(arena)) == ESP_OK, "arena 64 KB 就緒");
    CHECK(nc_plugin_set_backend(&FAKE) == ESP_OK, "backend 裝上");
    CHECK(nc_plugin_ready(), "工具就緒");
    CHECK(strcmp(nc_plugin_backend_name(), "fake") == 0, "backend 名 = fake");

    case_begin("載入 .nct → kind 以 TASK 傳到插件工具");
    const char payload[] = "hello-plugin";
    write_nct(path, payload, sizeof(payload) - 1);
    CHECK(nc_mod_load(path) == ESP_OK, "nc_mod_load 成功");
    CHECK(s_opens == 1, "工具 open 一次（實際 %d）", s_opens);
    CHECK(s_src.kind == NC_PLUGIN_KIND_TASK, "kind=TASK（實際 %u）", (unsigned)s_src.kind);
    CHECK(s_src.fmt == NC_PLUGIN_FMT_WASM, "fmt=wasm（實際 %u）", (unsigned)s_src.fmt);
    CHECK(s_src.size == sizeof(payload) - 1, "payload size=%u", (unsigned)s_src.size);
    CHECK(s_src.name != NULL && strcmp(s_src.name, "t") == 0, "name=t（實際 %s）",
          s_src.name ? s_src.name : "(null)");
    CHECK(nc_mod_live() != NULL, "第一個模組自動 select");

    case_begin("每 tick 一次 step，frame_id 遞增");
    nc_mod_tick();
    CHECK(s_steps == 1 && s_last_frame == 1, "tick 1 → steps=%d frame=%u",
          s_steps, (unsigned)s_last_frame);
    nc_mod_tick();
    CHECK(s_steps == 2 && s_last_frame == 2, "tick 2 → steps=%d frame=%u",
          s_steps, (unsigned)s_last_frame);

    case_begin("任務的 region 介面（同效果共用同一套）");
    // 任務：in = 參數（控制器寫）；out = 它交給別人的緩衝（狀態／日誌）。
    const nc_plugin_region_t regs[] = {
        { .dir = NC_PLUGIN_REGION_IN,  .align = 0, .name_id = 10, .bytes = 8 },
        { .dir = NC_PLUGIN_REGION_OUT, .align = 0, .name_id = 11, .bytes = 12 },
    };
    uint32_t offs[NC_PLUGIN_REGION_MAX] = {0};
    CHECK(nc_plugin_regions_bind(regs, 2, 4192, 65536, offs), "任務 2 個 region 放得落");
    CHECK(offs[0] == 4192 && offs[1] == 4208, "連排對齊（%u / %u）",
          (unsigned)offs[0], (unsigned)offs[1]);
    CHECK(!nc_plugin_regions_bind(regs, 2, 4192, 4200, offs), "放唔落 → 拒絕");

    case_begin("卸載 → close 一次；live 清空");
    CHECK(nc_mod_unload("t") == ESP_OK, "unload 成功");
    CHECK(s_closes == 1, "工具 close 一次（實際 %d）", s_closes);
    CHECK(nc_mod_live() == NULL, "live = NULL");

    case_begin("重新載入：arena 已回收，同一條路再走得通");
    CHECK(nc_mod_load(path) == ESP_OK, "第二次 nc_mod_load 成功");
    CHECK(s_opens == 2, "open 兩次（實際 %d）", s_opens);
    nc_mod_tick();
    CHECK(s_steps == 3, "step 三次（實際 %d）", s_steps);

    printf("\n%s（%d 個失敗）\n", s_fail ? "FAIL" : "ALL PASS", s_fail);
    return s_fail ? 1 : 0;
}
