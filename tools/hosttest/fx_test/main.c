// fx 狀態機的 host 測試（不需要板子、不需要 IDF）。
//
// 驗的是「安全邊界」那張表上的每一條：
//   壞檔被拒 / 載入成功 / render 失敗即停用 / 連續超支即停用 /
//   換檔兩幀後才釋放 / 全部卸載後 arena 回收 / 掃目錄 /
//   兩種格式（wasm 與 aot）與錯架構的 AOT 被擋
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "esp_rom_crc.h"
#include "fx_host.h"
#include "nc_fx.h"
#include "nc_fx_abi.h"

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

// ───────────────────────── 造 .fxe ─────────────────────────

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
} fxe_hdr_t;
#pragma pack(pop)

typedef struct {
    uint32_t magic;
    uint16_t abi;
    uint16_t flags;
    uint32_t target;
    int corrupt_crc;
    int corrupt_size;
} fxe_spec_t;

static void write_fxe(const char *path, fxe_spec_t spec,
                      const unsigned char *payload, size_t n)
{
    fxe_hdr_t h = {
        .magic = spec.magic,
        .abi = spec.abi,
        .flags = spec.flags,
        .total_size = (uint32_t)(sizeof(h) + n),
        .payload_off = (uint32_t)sizeof(h),
        .payload_size = (uint32_t)n,
        .payload_crc = 0,
        .target_id = spec.target,
        .mem_bytes = 65536,
        .px = 676,
        .fps = 30,
    };
    // 與裝置端相同的 crc 慣例（init=0）
    h.payload_crc = esp_rom_crc32_le(0, payload, (unsigned int)n);
    if (spec.corrupt_crc) {
        h.payload_crc ^= 0xFFFFFFFFu;
    }
    if (spec.corrupt_size) {
        h.total_size -= 10;          // payload 超出總長 → 應被拒
    }

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

enum { RENDER_OK = 0, RENDER_FAIL = 1, RENDER_SLOW = 2 };

static int s_mode = RENDER_OK;
static int s_renders;
static uint32_t s_last_frame;

static esp_err_t fake_render(nc_fx_t *fx, uint32_t frame_id)
{
    (void)fx;
    s_renders++;
    s_last_frame = frame_id;
    if (s_mode == RENDER_FAIL) {
        return ESP_FAIL;
    }
    if (s_mode == RENDER_SLOW) {
        struct timespec ts = { 0, 17 * 1000 * 1000 };   // 17 ms > 16 ms 預算
        nanosleep(&ts, NULL);
    }
    return ESP_OK;
}

static esp_err_t fake_init(void *arena, size_t bytes)
{
    (void)arena;
    (void)bytes;
    return ESP_OK;
}

static const nc_fx_backend_t FAKE = {
    .name = "fake",
    .init = fake_init,
    .render = fake_render,
    .deinit = NULL,
};

// ───────────────────────── 小工具 ─────────────────────────

static int count_modules(void)
{
    char buf[1024];
    nc_fx_status_json(buf, sizeof(buf));
    int n = 0;
    for (const char *p = buf; (p = strstr(p, "\"name\":")) != NULL; p++) {
        n++;
    }
    return n;
}

static char *status(void)
{
    static char buf[1024];
    nc_fx_status_json(buf, sizeof(buf));
    return buf;
}

static void unload_all(void)
{
    nc_fx_select(NULL);
    for (int i = 0; i < NC_FX_MAX_MODULES; i++) {
        char buf[1024];
        snprintf(buf, sizeof(buf), "%s", status());
        const char *p = strstr(buf, "\"name\":\"");
        if (p == NULL) {
            break;
        }
        char name[32] = {0};
        if (sscanf(p + 8, "%31[^\"]", name) == 1) {
            nc_fx_unload(name);
        } else {
            break;
        }
    }
}

// ───────────────────────── 測項 ─────────────────────────

int main(void)
{
    static unsigned char arena[64 * 1024];
    char dir[256];
    const char *tmp = getenv("TMPDIR");
    snprintf(dir, sizeof(dir), "%s/fx_test_mods", tmp ? tmp : "/tmp");
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s'", dir, dir);
    if (system(cmd) != 0) {
        fprintf(stderr, "建立測試目錄失敗：%s\n", dir);
        return 2;
    }

    const unsigned char payload[] = { 'W', 'A', 'S', 'M', 1, 2, 3, 4, 5, 6, 7, 8 };
    const size_t plen = sizeof(payload);

    char good[256], other[256], bad_magic[256], bad_crc[256], bad_size[256];
    char aot_ok[256], aot_wrong[256], aot_no_target[256], wasm_with_target[256];
    snprintf(good, sizeof(good), "%s/fire26.fxe", dir);
    snprintf(other, sizeof(other), "%s/plasma.fxe", dir);
    snprintf(bad_magic, sizeof(bad_magic), "%s/badmagic.fxe", dir);
    snprintf(bad_crc, sizeof(bad_crc), "%s/badcrc.fxe", dir);
    snprintf(bad_size, sizeof(bad_size), "%s/badsize.fxe", dir);
    snprintf(aot_ok, sizeof(aot_ok), "%s/aot_here.fxe", dir);
    snprintf(aot_wrong, sizeof(aot_wrong), "%s/aot_other.fxe", dir);
    snprintf(aot_no_target, sizeof(aot_no_target), "%s/aot_zero.fxe", dir);
    snprintf(wasm_with_target, sizeof(wasm_with_target), "%s/wasm_target.fxe", dir);

    write_fxe(good, (fxe_spec_t){ .magic = 0x31455846u }, payload, plen);
    write_fxe(other, (fxe_spec_t){ .magic = 0x31455846u }, payload, plen);
    write_fxe(bad_magic, (fxe_spec_t){ .magic = 0xDEADBEEFu }, payload, plen);
    write_fxe(bad_crc, (fxe_spec_t){ .magic = 0x31455846u, .corrupt_crc = 1 }, payload, plen);
    write_fxe(bad_size, (fxe_spec_t){ .magic = 0x31455846u, .corrupt_size = 1 }, payload, plen);

    case_begin("init");
    CHECK(nc_fx_init(NULL, 0) == ESP_ERR_NO_MEM, "沒有 arena → 明確失敗");
    CHECK(nc_fx_init(arena, sizeof(arena)) == ESP_OK, "arena 64 KB 就緒");
    CHECK(nc_fx_set_backend(&FAKE) == ESP_OK, "backend 裝上");

    case_begin("壞檔全部被拒，且不留空槽");
    CHECK(nc_fx_load(bad_magic) == ESP_ERR_INVALID_ARG, "magic 錯 → ERR_INVALID_ARG");
    CHECK(nc_fx_load(bad_crc) == ESP_ERR_INVALID_CRC, "crc 錯 → ERR_INVALID_CRC");
    CHECK(nc_fx_load(bad_size) == ESP_ERR_INVALID_SIZE, "長度錯 → ERR_INVALID_SIZE");
    CHECK(nc_fx_load("/tmp/does-not-exist.fxe") == ESP_ERR_NOT_FOUND, "不存在 → ERR_NOT_FOUND");
    CHECK(count_modules() == 0, "實例表仍然是 0");

    case_begin("載入正常檔 → 自動上線");
    CHECK(nc_fx_load(good) == ESP_OK, "載入 fire26（wasm）");
    CHECK(nc_fx_live() != NULL, "第一個自動 select");
    CHECK(strstr(status(), "\"live\":\"fire26\"") != NULL, "status 顯示 live=fire26");
    CHECK(strstr(status(), "\"fmt\":\"wasm\"") != NULL, "格式是 wasm");
    CHECK(nc_fx_load(good) == ESP_ERR_INVALID_STATE, "重複載入被拒");

    case_begin("每幀 render 被呼叫");
    s_renders = 0;
    nc_fx_tick();
    nc_fx_tick();
    CHECK(s_renders == 2, "兩幀 → render 2 次（實際 %d）", s_renders);
    CHECK(s_last_frame == 2, "frame_id 遞增到 2（實際 %u）", (unsigned)s_last_frame);

    case_begin("render 回錯誤 → 立刻停用，核心不受影響");
    s_mode = RENDER_FAIL;
    nc_fx_tick();
    CHECK(nc_fx_live() == NULL, "live 被清空");
    CHECK(strstr(status(), "\"bad\":1") != NULL, "模組標記 bad");
    CHECK(nc_fx_select("fire26") == ESP_ERR_INVALID_STATE, "bad 的模組不能再選");
    s_mode = RENDER_OK;
    unload_all();
    CHECK(count_modules() == 0, "卸載後實例表清空");

    case_begin("連續超支 3 次 → 停用（前兩次只記 strike）");
    CHECK(nc_fx_load(other) == ESP_OK, "載入 plasma");
    s_mode = RENDER_SLOW;
    nc_fx_tick();
    CHECK(strstr(status(), "\"strikes\":1") != NULL, "第 1 次超支 → strike=1");
    CHECK(nc_fx_live() != NULL, "第 1 次之後仍然在跑");
    nc_fx_tick();
    CHECK(strstr(status(), "\"strikes\":2") != NULL, "第 2 次 → strike=2");
    CHECK(nc_fx_live() != NULL, "第 2 次之後仍然在跑");
    nc_fx_tick();
    CHECK(nc_fx_live() == NULL, "第 3 次 → 停用");
    CHECK(strstr(status(), "\"bad\":1") != NULL, "標記 bad");
    s_mode = RENDER_OK;
    unload_all();

    case_begin("換檔：舊的要兩幀後才釋放");
    CHECK(nc_fx_load(good) == ESP_OK, "載入 A");
    CHECK(nc_fx_load(other) == ESP_OK, "載入 B（A 仍是 live）");
    CHECK(count_modules() == 2, "兩個都在表上");
    CHECK(strstr(status(), "\"live\":\"fire26\"") != NULL, "live 仍然是 A");
    CHECK(nc_fx_select("plasma") == ESP_OK, "切到 B");
    CHECK(strstr(status(), "\"live\":\"plasma\"") != NULL, "live → plasma");
    nc_fx_tick();
    CHECK(count_modules() == 2, "第 1 幀後 A 還在（可能有人正握著指標）");
    nc_fx_tick();
    CHECK(count_modules() == 1, "第 2 幀後 A 才被釋放（實際 %d 個）", count_modules());
    unload_all();

    case_begin("兩種格式：wasm 與 aot 都收");
    CHECK(nc_fx_arch_target() == NC_FX_TARGET_XTENSA, "host 假裝是 Xtensa（%d）", nc_fx_arch_target());
    CHECK(nc_fx_load(good) == ESP_OK, "wasm 收");
    CHECK(strstr(status(), "\"fmt\":\"wasm\"") != NULL, "fmt=wasm");
    write_fxe(aot_ok, (fxe_spec_t){ .magic = 0x31455846u, .flags = 0x0001,
                                    .target = NC_FX_TARGET_XTENSA }, payload, plen);
    CHECK(nc_fx_load(aot_ok) == ESP_OK, "aot（target=Xtensa）收");
    CHECK(strstr(status(), "\"fmt\":\"aot\"") != NULL, "fmt=aot");
    unload_all();

    case_begin("錯架構的 AOT 被擋（不是 illegal instruction）");
    write_fxe(aot_wrong, (fxe_spec_t){ .magic = 0x31455846u, .flags = 0x0001,
                                       .target = NC_FX_TARGET_RV32F }, payload, plen);
    CHECK(nc_fx_load(aot_wrong) == ESP_ERR_NOT_SUPPORTED,
          "riscv32 的 AOT 在 Xtensa 上 → ERR_NOT_SUPPORTED");
    CHECK(count_modules() == 0, "沒有留下半個實例");

    case_begin("格式與 target 自相矛盾 → 拒絕");
    write_fxe(aot_no_target, (fxe_spec_t){ .magic = 0x31455846u, .flags = 0x0001,
                                           .target = NC_FX_TARGET_WASM }, payload, plen);
    CHECK(nc_fx_load(aot_no_target) == ESP_ERR_INVALID_ARG, "標 AOT 卻 target=0");
    write_fxe(wasm_with_target, (fxe_spec_t){ .magic = 0x31455846u,
                                              .target = NC_FX_TARGET_XTENSA }, payload, plen);
    CHECK(nc_fx_load(wasm_with_target) == ESP_ERR_INVALID_ARG, "沒標 AOT 卻寫 target=2");

    case_begin("晶片架構未知時只收 wasm");
    fx_host_arch_target = 0;
    CHECK(nc_fx_load(aot_ok) == ESP_ERR_NOT_SUPPORTED, "arch 未知 → AOT 一律拒絕");
    CHECK(nc_fx_load(good) == ESP_OK, "wasm 照收");
    unload_all();
    fx_host_arch_target = NC_FX_TARGET_XTENSA;

    case_begin("全部卸載 → arena 歸零");
    unload_all();
    CHECK(strstr(status(), "\"used\":0") != NULL, "arena used 回到 0");

    case_begin("實例表上限");
    for (int i = 0; i < 10; i++) {
        char p[256];
        snprintf(p, sizeof(p), "%s/g%d.fxe", dir, i);
        write_fxe(p, (fxe_spec_t){ .magic = 0x31455846u }, payload, plen);
    }
    CHECK(nc_fx_scan(dir) > 0, "掃目錄有東西");
    CHECK(count_modules() == NC_FX_MAX_MODULES,
          "最多 %d 個（實際 %d）", NC_FX_MAX_MODULES, count_modules());

    printf("\n%s（%d 個失敗）\n", s_fail ? "FAILED" : "ALL PASS", s_fail);
    return s_fail ? 1 : 0;
}
