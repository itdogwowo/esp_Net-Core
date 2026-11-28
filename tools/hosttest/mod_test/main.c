// ncm 狀態機的 host 測試（不需要板子、不需要 IDF）。
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
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "esp_rom_crc.h"
#include "mod_host.h"
#include "nc_mod.h"
#include "nc_plugin.h"
#include "nc_mod_abi.h"
#include "nc_ncd.h"
#include "nc_res.h"

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

// ───────────────────────── 造 .nce ─────────────────────────

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

typedef struct {
    uint32_t magic;
    uint16_t abi;
    uint16_t flags;
    uint32_t target;
    int corrupt_crc;
    int corrupt_size;
} ncm_spec_t;

static void write_ncm(const char *path, ncm_spec_t spec,
                      const unsigned char *payload, size_t n)
{
    ncm_hdr_t h = {
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

static esp_err_t fake_step(nc_plugin_t plugin, uint32_t frame_id)
{
    (void)plugin;
    s_renders++;
    s_last_frame = frame_id;
    if (s_mode == RENDER_FAIL) {
        return ESP_FAIL;
    }
    if (s_mode == RENDER_SLOW) {
#ifdef _WIN32
        Sleep(17);
#else
        struct timespec ts = { 0, 17 * 1000 * 1000 };   // 17 ms > 16 ms 預算
        nanosleep(&ts, NULL);
#endif
    }
    return ESP_OK;
}

static esp_err_t fake_init(void *arena, size_t bytes)
{
    (void)arena;
    (void)bytes;
    return ESP_OK;
}

static esp_err_t fake_open(nc_plugin_t *out, const nc_plugin_src_t *src)
{
    (void)src;
    *out = (void *)(uintptr_t)0x1;        // 假把手（唔會 deref）
    return ESP_OK;
}

static void fake_close(nc_plugin_t plugin)
{
    (void)plugin;
}

static const nc_plugin_backend_t FAKE = {
    .name = "fake",
    .init = fake_init,
    .open = fake_open,
    .step = fake_step,
    .close = fake_close,
    .deinit = NULL,
};

// ───────────────────────── 小工具 ─────────────────────────

static int count_modules(void)
{
    /* 每個模組大約 250 B 的 JSON，8 個就 2,000 B —— 用 1,024 會截斷，
     * 於是「數到 5 個」其實是緩衝不夠，不是固件少收了。 */
    char buf[4096];
    nc_mod_status_json(buf, sizeof(buf));
    int n = 0;
    for (const char *p = buf; (p = strstr(p, "\"name\":")) != NULL; p++) {
        n++;
    }
    return n;
}

static char *status(void)
{
    static char buf[1024];
    nc_mod_status_json(buf, sizeof(buf));
    return buf;
}

static void unload_all(void)
{
    nc_mod_select(NULL);
    for (int i = 0; i < NC_MOD_MAX_MODULES; i++) {
        /* 用 4 KB：1 KB 會令 nc_mod_info_json 走「truncated」精簡路徑，
         * 裡面沒有模組名 → unload_all 會以為已經清空。 */
        char buf[4096];
        nc_mod_status_json(buf, sizeof(buf));
        const char *p = strstr(buf, "\"name\":\"");
        if (p == NULL) {
            break;
        }
        char name[32] = {0};
        if (sscanf(p + 8, "%31[^\"]", name) == 1) {
            nc_mod_unload(name);
        } else {
            break;
        }
    }
}

// ───────────────────────── 測項 ─────────────────────────

int main(void)
{
    static unsigned char arena[64 * 1024];
    /* dir 刻意只開 200：gcc 的 -Wformat-truncation 是看「陣列宣告大小」去估上限，
     * 開 256 的話 "%s/xxx.nce" 砌入 char[256] 就「可能」截斷，-Werror 直接當錯。
     * 200 之後所有 256 的目標都證明放得落（199 + 最長 17 + 1）。 */
    char dir[200];
    const char *tmp = getenv("TMPDIR");
#ifdef _WIN32
    if (tmp == NULL) {
        tmp = getenv("TEMP");
    }
#endif
    snprintf(dir, sizeof(dir), "%.180s/mod_test_mods", tmp ? tmp : "/tmp");
#ifdef _WIN32
    for (char *p = dir; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\\';
        }
    }
#endif
    char cmd[600];
#ifdef _WIN32
    snprintf(cmd, sizeof(cmd), "rmdir /s /q \"%s\" 2>nul", dir);
    (void)system(cmd);
    if (_mkdir(dir) != 0) {
        fprintf(stderr, "建立測試目錄失敗：%s\n", dir);
        return 2;
    }
#else
    snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s'", dir, dir);
    if (system(cmd) != 0) {
        fprintf(stderr, "建立測試目錄失敗：%s\n", dir);
        return 2;
    }
#endif

    const unsigned char payload[] = { 'W', 'A', 'S', 'M', 1, 2, 3, 4, 5, 6, 7, 8 };
    const size_t plen = sizeof(payload);

    char good[256], other[256], bad_magic[256], bad_crc[256], bad_size[256];
    char aot_ok[256], aot_wrong[256], aot_no_target[256], wasm_with_target[256];
    snprintf(good, sizeof(good), "%s/fire26.nce", dir);
    snprintf(other, sizeof(other), "%s/plasma.nce", dir);
    snprintf(bad_magic, sizeof(bad_magic), "%s/badmagic.nce", dir);
    snprintf(bad_crc, sizeof(bad_crc), "%s/badcrc.nce", dir);
    snprintf(bad_size, sizeof(bad_size), "%s/badsize.nce", dir);
    snprintf(aot_ok, sizeof(aot_ok), "%s/aot_here.nce", dir);
    snprintf(aot_wrong, sizeof(aot_wrong), "%s/aot_other.nce", dir);
    snprintf(aot_no_target, sizeof(aot_no_target), "%s/aot_zero.nce", dir);
    snprintf(wasm_with_target, sizeof(wasm_with_target), "%s/wasm_target.nce", dir);

    write_ncm(good, (ncm_spec_t){ .magic = 0x314D434Eu }, payload, plen);
    write_ncm(other, (ncm_spec_t){ .magic = 0x314D434Eu }, payload, plen);
    write_ncm(bad_magic, (ncm_spec_t){ .magic = 0xDEADBEEFu }, payload, plen);
    write_ncm(bad_crc, (ncm_spec_t){ .magic = 0x314D434Eu, .corrupt_crc = 1 }, payload, plen);
    write_ncm(bad_size, (ncm_spec_t){ .magic = 0x314D434Eu, .corrupt_size = 1 }, payload, plen);

    char task_only[256];
    snprintf(task_only, sizeof(task_only), "%s/task_only.nct", dir);
    write_ncm(task_only, (ncm_spec_t){ .magic = 0x314D434Eu }, payload, plen);

    case_begin("probe：只讀檔頭 + manifest，不需要 arena");
    {
        nc_mod_probe_t pr;
        CHECK(nc_mod_probe(task_only, &pr) == ESP_OK, "probe .nct 成功");
        CHECK(pr.decl.kind == NC_NCD_KIND_TASK, "沒有 manifest → .nct = task");
        CHECK(pr.decl.has_manifest == false, "has_manifest=0");
        CHECK(strcmp(pr.name, "task_only") == 0, "name 來自檔名");
        CHECK(nc_mod_probe(good, &pr) == ESP_OK, "probe .nce 成功");
        CHECK(pr.decl.kind == NC_NCD_KIND_EFFECT, "沒有 manifest → .nce = effect");
        CHECK(nc_mod_probe(bad_magic, &pr) == ESP_ERR_INVALID_ARG, "壞 magic → 拒絕");
    }

    case_begin("init");
    // arena 大小由掃描結果決定（2026-11-27）：唔再無條件預留 512 KB。
    printf("[arena] 掃描量 → arena bytes\n");
    {
        nc_mod_scan_t sc;
        memset(&sc, 0, sizeof(sc));
        CHECK(nc_mod_arena_bytes(&sc) == 0, "冇模組 → 0 B（唔再預留 32 KB）");
        CHECK(nc_mod_arena_bytes(NULL) == 0, "NULL → 0 B");
        sc.count = 2;
        sc.bytes = 5000;
        const size_t ab = nc_mod_arena_bytes(&sc);
        CHECK(ab >= 5000u + 2u * 1024u && ab <= NC_MOD_ARENA_MAX && (ab % 32u) == 0,
              "2 個模組 5,000 B → 合計＋2 KB、32 對齊（%u）", (unsigned)ab);
        sc.count = NC_MOD_MAX_ENTRIES;
        sc.bytes = (size_t)NC_MOD_ARENA_MAX * 2u;
        CHECK(nc_mod_arena_bytes(&sc) == NC_MOD_ARENA_MAX, "超出上限 → MAX");
    }

    CHECK(nc_mod_init(NULL, 0) == ESP_ERR_NO_MEM, "沒有 arena → 明確失敗");
    CHECK(nc_mod_init(arena, sizeof(arena)) == ESP_OK, "arena 64 KB 就緒");
    CHECK(nc_plugin_set_backend(&FAKE) == ESP_OK, "backend 裝上");

    case_begin("壞檔全部被拒，且不留空槽");
    CHECK(nc_mod_load(bad_magic) == ESP_ERR_INVALID_ARG, "magic 錯 → ERR_INVALID_ARG");
    CHECK(nc_mod_load(bad_crc) == ESP_ERR_INVALID_CRC, "crc 錯 → ERR_INVALID_CRC");
    CHECK(nc_mod_load(bad_size) == ESP_ERR_INVALID_SIZE, "長度錯 → ERR_INVALID_SIZE");
    CHECK(nc_mod_load("/tmp/does-not-exist.nce") == ESP_ERR_NOT_FOUND, "不存在 → ERR_NOT_FOUND");
    CHECK(count_modules() == 0, "實例表仍然是 0");

    case_begin("載入正常檔 → 自動上線");
    CHECK(nc_mod_load(good) == ESP_OK, "載入 fire26（wasm）");
    CHECK(nc_mod_live() != NULL, "第一個自動 select");
    CHECK(strstr(status(), "\"live\":\"fire26\"") != NULL, "status 顯示 live=fire26");
    CHECK(strstr(status(), "\"fmt\":\"wasm\"") != NULL, "格式是 wasm");
    CHECK(nc_mod_load(good) == ESP_ERR_INVALID_STATE, "重複載入被拒");

    case_begin("每幀 render 被呼叫");
    s_renders = 0;
    nc_mod_tick();
    nc_mod_tick();
    CHECK(s_renders == 2, "兩幀 → render 2 次（實際 %d）", s_renders);
    CHECK(s_last_frame == 2, "frame_id 遞增到 2（實際 %u）", (unsigned)s_last_frame);

    case_begin("render 回錯誤 → 立刻停用，核心不受影響");
    s_mode = RENDER_FAIL;
    nc_mod_tick();
    CHECK(nc_mod_live() == NULL, "live 被清空");
    CHECK(strstr(status(), "\"bad\":1") != NULL, "模組標記 bad");
    CHECK(nc_mod_select("fire26") == ESP_ERR_INVALID_STATE, "bad 的模組不能再選");
    s_mode = RENDER_OK;
    unload_all();
    CHECK(count_modules() == 0, "卸載後實例表清空");

    case_begin("連續超支 3 次 → 停用（前兩次只記 strike）");
    CHECK(nc_mod_load(other) == ESP_OK, "載入 plasma");
    s_mode = RENDER_SLOW;
    nc_mod_tick();
    CHECK(strstr(status(), "\"strikes\":1") != NULL, "第 1 次超支 → strike=1");
    CHECK(nc_mod_live() != NULL, "第 1 次之後仍然在跑");
    nc_mod_tick();
    CHECK(strstr(status(), "\"strikes\":2") != NULL, "第 2 次 → strike=2");
    CHECK(nc_mod_live() != NULL, "第 2 次之後仍然在跑");
    nc_mod_tick();
    CHECK(nc_mod_live() == NULL, "第 3 次 → 停用");
    CHECK(strstr(status(), "\"bad\":1") != NULL, "標記 bad");
    s_mode = RENDER_OK;
    unload_all();

    case_begin("換檔：舊的要兩幀後才釋放");
    CHECK(nc_mod_load(good) == ESP_OK, "載入 A");
    CHECK(nc_mod_load(other) == ESP_OK, "載入 B（A 仍是 live）");
    CHECK(count_modules() == 2, "兩個都在表上");
    CHECK(strstr(status(), "\"live\":\"fire26\"") != NULL, "live 仍然是 A");
    CHECK(nc_mod_select("plasma") == ESP_OK, "切到 B");
    CHECK(strstr(status(), "\"live\":\"plasma\"") != NULL, "live → plasma");
    nc_mod_tick();
    CHECK(count_modules() == 2, "第 1 幀後 A 還在（可能有人正握著指標）");
    nc_mod_tick();
    CHECK(count_modules() == 1, "第 2 幀後 A 才被釋放（實際 %d 個）", count_modules());
    unload_all();

    case_begin("兩種格式：wasm 與 aot 都收");
    CHECK(nc_mod_arch_target() == NC_MOD_TARGET_XTENSA, "host 假裝是 Xtensa（%d）", nc_mod_arch_target());
    CHECK(nc_mod_load(good) == ESP_OK, "wasm 收");
    CHECK(strstr(status(), "\"fmt\":\"wasm\"") != NULL, "fmt=wasm");
    write_ncm(aot_ok, (ncm_spec_t){ .magic = 0x314D434Eu, .flags = 0x0001,
                                    .target = NC_MOD_TARGET_XTENSA }, payload, plen);
    CHECK(nc_mod_load(aot_ok) == ESP_OK, "aot（target=Xtensa）收");
    CHECK(strstr(status(), "\"fmt\":\"aot\"") != NULL, "fmt=aot");
    unload_all();

    case_begin("錯架構的 AOT 被擋（不是 illegal instruction）");
    write_ncm(aot_wrong, (ncm_spec_t){ .magic = 0x314D434Eu, .flags = 0x0001,
                                       .target = NC_MOD_TARGET_RV32F }, payload, plen);
    CHECK(nc_mod_load(aot_wrong) == ESP_ERR_NOT_SUPPORTED,
          "riscv32 的 AOT 在 Xtensa 上 → ERR_NOT_SUPPORTED");
    CHECK(count_modules() == 0, "沒有留下半個實例");

    case_begin("格式與 target 自相矛盾 → 拒絕");
    write_ncm(aot_no_target, (ncm_spec_t){ .magic = 0x314D434Eu, .flags = 0x0001,
                                           .target = NC_MOD_TARGET_WASM }, payload, plen);
    CHECK(nc_mod_load(aot_no_target) == ESP_ERR_INVALID_ARG, "標 AOT 卻 target=0");
    write_ncm(wasm_with_target, (ncm_spec_t){ .magic = 0x314D434Eu,
                                              .target = NC_MOD_TARGET_XTENSA }, payload, plen);
    CHECK(nc_mod_load(wasm_with_target) == ESP_ERR_INVALID_ARG, "沒標 AOT 卻寫 target=2");

    case_begin("晶片架構未知時只收 wasm");
    mod_host_arch_target = 0;
    CHECK(nc_mod_load(aot_ok) == ESP_ERR_NOT_SUPPORTED, "arch 未知 → AOT 一律拒絕");
    CHECK(nc_mod_load(good) == ESP_OK, "wasm 照收");
    unload_all();
    mod_host_arch_target = NC_MOD_TARGET_XTENSA;

    case_begin("全部卸載 → arena 歸零");
    unload_all();
    CHECK(strstr(status(), "\"used\":0") != NULL, "arena used 回到 0");

    case_begin("實例表上限");
    for (int i = 0; i < 10; i++) {
        char p[256];
        snprintf(p, sizeof(p), "%s/g%d.nce", dir, i);
        write_ncm(p, (ncm_spec_t){ .magic = 0x314D434Eu }, payload, plen);
    }
    CHECK(nc_mod_scan(dir) > 0, "掃目錄有東西");
    CHECK(count_modules() == NC_MOD_MAX_MODULES,
          "最多 %d 個（實際 %d）", NC_MOD_MAX_MODULES, count_modules());

    case_begin("框架：scan_dir → merge → load_all");
    {
        unload_all();
        CHECK(count_modules() == 0, "unload_all 清空（實際 %d）", count_modules());
        char dir2[208];   // dir[200] + "/fw"：gcc 13 -Wformat-truncation
        snprintf(dir2, sizeof(dir2), "%s/fw", dir);
        char cmd2[600];
#ifdef _WIN32
        snprintf(cmd2, sizeof(cmd2), "mkdir \"%s\" 2>nul", dir2);
#else
        snprintf(cmd2, sizeof(cmd2), "mkdir -p '%s'", dir2);
#endif
        (void)system(cmd2);

        char f1[256], f2[256];
        snprintf(f1, sizeof(f1), "%s/a.nct", dir2);
        snprintf(f2, sizeof(f2), "%s/b.nce", dir2);
        write_ncm(f1, (ncm_spec_t){ .magic = 0x314D434Eu }, payload, plen);
        write_ncm(f2, (ncm_spec_t){ .magic = 0x314D434Eu }, payload, plen);

        nc_mod_scan_t scan;
        memset(&scan, 0, sizeof(scan));
        CHECK(nc_mod_scan_dir(dir2, &scan) == 2, "scan_dir 找到 2 個");
        CHECK(scan.count == 2, "scan.count=2");
        CHECK(nc_mod_merge_cfg(&scan) == 2, "merge 處理 2 個");
        CHECK(scan.entries[0].enabled && scan.entries[1].enabled, "兩個都 enabled");
        CHECK(nc_mod_load_all(&scan) == 2, "load_all 載入 2 個");
        unload_all();
    }

    case_begin("資源表：requires 缺 → 拒絕；補齊 → 通過");
    {
        nc_res_reset();
        nc_res_add_prov("led.frame", NC_RES_BUF, "pixel", 2704, false);
        nc_res_add_req("led.frame", NC_RES_BUF, "fire26", 2704, false);
        nc_res_add_req("rmt", NC_RES_HW, "fire26", 0, true);
        nc_res_merge();
        char why[96];
        CHECK(!nc_res_check("fire26", why, sizeof(why)), "缺 rmt → 拒絕");
        CHECK(strstr(why, "rmt") != NULL, "why 提到 rmt（實際：%s）", why);
        nc_res_add_prov("rmt", NC_RES_HW, "pixel", 0, true);
        nc_res_merge();
        CHECK(nc_res_check("fire26", why, sizeof(why)), "補 rmt → 通過");
        CHECK(nc_res_check("other", why, sizeof(why)), "沒有 requires 的 owner → 通過");
    }

    printf("\n%s（%d 個失敗）\n", s_fail ? "FAILED" : "ALL PASS", s_fail);
    return s_fail ? 1 : 0;
}
