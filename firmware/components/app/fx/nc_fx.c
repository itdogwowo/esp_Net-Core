#include "nc_fx.h"
#include "nc_fx_abi.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifdef NC_FX_HOST_TEST
// ── host 測試（Mac 上 cc 直接編）：平台東西全部換成 shim ──
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "fx_host.h"          // fx_host_now_us()
#define fx_now_us() fx_host_now_us()
#else
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "nc_plan.h"
#define fx_now_us() esp_timer_get_time()
#endif

static const char *TAG = "fx";

// 這顆晶片吃的 AOT target。0 = 未知（這種情況只收 wasm）。
#ifdef NC_FX_HOST_TEST
int fx_host_arch_target = NC_FX_TARGET_XTENSA;   // 測試可改
int nc_fx_arch_target(void) { return fx_host_arch_target; }
#else
int nc_fx_arch_target(void)
{
#if CONFIG_IDF_TARGET_ESP32P4
    return NC_FX_TARGET_RV32F;      // P4 = RISC-V，且只有單精度 F（不能 ilp32d）
#elif CONFIG_IDF_TARGET_ESP32S3
    return NC_FX_TARGET_XTENSA;
#else
    return 0;
#endif
}
#endif

// ── .fxe 檔頭（48 B，見 fx-loader.md §3）──
#define FXE_MAGIC        0x31455846u      // 'F','X','E','1' little-endian
#define FXE_HDR_SIZE     48
#define FXE_ABI          0
#define FXE_FLAG_AOT     0x0001
#define FXE_FLAG_MANIFEST 0x0002

typedef struct __attribute__((packed)) {
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
} nc_fxe_hdr_t;

_Static_assert(sizeof(nc_fxe_hdr_t) == FXE_HDR_SIZE, "fxe header must stay 48 B");
_Static_assert(sizeof(nc_fx_desc_t) == 28, "fx descriptor must stay 28 B");
_Static_assert(NC_FX_PARAM_OFF == 32 && NC_FX_DATA_OFF == 96, "fx layout is pinned by abi v0");

struct nc_fx {
    char     name[NC_FX_NAME_MAX];
    char     path[NC_FX_PATH_MAX];
    uint8_t *payload;          // 指向 arena 內
    size_t   payload_size;
    uint32_t mem_bytes;
    uint32_t px;
    uint32_t fps;
    uint16_t flags;
    uint8_t  fmt;              // NC_FX_FMT_WASM / NC_FX_FMT_AOT
    bool     used;
    bool     ok;               // 驗證通過、且有執行引擎
    bool     bad;              // trap 或連續超支 → 不再選用
    uint8_t  strikes;          // 連續超支次數
};

// ── 狀態 ──
static uint8_t *s_arena;
static size_t   s_arena_bytes;
static size_t   s_arena_used;
static nc_fx_t  s_mods[NC_FX_MAX_MODULES];
static nc_fx_t *volatile s_live;
static volatile uint32_t s_frame;
static char     s_retire[NC_FX_NAME_MAX];   // 等兩幀才釋放的模組
static uint32_t s_retire_at;
static const nc_fx_backend_t *s_backend;

// ───────────────────────── 小工具 ─────────────────────────

static const char *base_name(const char *path, char *out, size_t cap)
{
    const char *slash = strrchr(path, '/');
    const char *p = slash ? slash + 1 : path;
    const char *dot = strrchr(p, '.');
    size_t n = dot ? (size_t)(dot - p) : strlen(p);
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(out, p, n);
    out[n] = '\0';
    return out;
}

static nc_fx_t *find_mod(const char *name)
{
    for (int i = 0; i < NC_FX_MAX_MODULES; i++) {
        if (s_mods[i].used && strcmp(s_mods[i].name, name) == 0) {
            return &s_mods[i];
        }
    }
    return NULL;
}

// bump allocator：沒有逐塊 free，最後一個模組卸載時整個 arena 回收。
// （文件寫的是 arena mark / reset，不是通用 malloc —— 這裡照那個語義。）
static void *arena_alloc(size_t bytes, size_t align)
{
    if (s_arena == NULL || bytes == 0) {
        return NULL;
    }
    if (align < 8) {
        align = 8;
    }
    const size_t off = (s_arena_used + align - 1) & ~(align - 1);
    if (off + bytes > s_arena_bytes) {
        return NULL;
    }
    s_arena_used = off + bytes;
    return s_arena + off;
}

static void arena_reset_if_empty(void)
{
    for (int i = 0; i < NC_FX_MAX_MODULES; i++) {
        if (s_mods[i].used) {
            return;
        }
    }
    s_arena_used = 0;
    ESP_LOGI(TAG, "arena 已回收（%u B）", (unsigned)s_arena_bytes);
}

// backend 決定「能不能跑」。載入時就判斷，之後改 backend 要重算。
static void refresh_ok(void)
{
    for (int i = 0; i < NC_FX_MAX_MODULES; i++) {
        if (s_mods[i].used) {
            s_mods[i].ok = (s_backend != NULL) && !s_mods[i].bad;
        }
    }
}

// ───────────────────────── 生命週期 ─────────────────────────

esp_err_t nc_fx_declare(void)
{
#ifdef NC_FX_HOST_TEST
    return ESP_OK;                 // host 測試不經過 nc_plan
#else
    return nc_plan_declare(&(nc_plan_decl_t){
        .service = "fx",
        .name = "fx.arena",
        .level = 4,
        .cls = NC_PLAN_PSRAM,
        .bytes = NC_FX_ARENA_MAX,
        .align = 32,
    });
#endif
}

esp_err_t nc_fx_init(void *arena, size_t bytes)
{
    s_arena = (uint8_t *)arena;
    s_arena_bytes = bytes;
    s_arena_used = 0;
    s_live = NULL;
    s_frame = 0;
    s_retire[0] = '\0';
    memset(s_mods, 0, sizeof(s_mods));

    if (s_arena == NULL) {
        ESP_LOGW(TAG, "拿不到 arena（plan 沒給）→ fx 停用");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "arena %u B（backend=%s）",
             (unsigned)bytes, s_backend ? s_backend->name : "none");
    return ESP_OK;
}

// 之後接 WAMR 或 host 測試用；不能在 render 中途換（loader 端負責時機）。
esp_err_t nc_fx_set_backend(const nc_fx_backend_t *backend)
{
    s_backend = backend;
    if (backend != NULL && backend->init != NULL) {
        const esp_err_t err = backend->init(s_arena, s_arena_bytes);
        if (err != ESP_OK) {
            s_backend = NULL;
            return err;
        }
    }
    refresh_ok();
    return ESP_OK;
}

// ───────────────────────── 載入 / 卸載 ─────────────────────────

esp_err_t nc_fx_load(const char *path)
{
    if (s_arena == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char name[NC_FX_NAME_MAX];
    base_name(path, name, sizeof(name));
    if (find_mod(name) != NULL) {
        return ESP_ERR_INVALID_STATE;      // 已載入
    }

    nc_fx_t *slot = NULL;
    for (int i = 0; i < NC_FX_MAX_MODULES; i++) {
        if (!s_mods[i].used) {
            slot = &s_mods[i];
            break;
        }
    }
    if (slot == NULL) {
        ESP_LOGW(TAG, "實例表滿了（%d）", NC_FX_MAX_MODULES);
        return ESP_ERR_NO_MEM;
    }

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGW(TAG, "開不了 %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    nc_fxe_hdr_t hdr;
    if (fread(&hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }

    // ── 驗證（六步的前三步）──
    if (hdr.magic != FXE_MAGIC) {
        ESP_LOGW(TAG, "%s：magic 不對（0x%08x）", name, (unsigned)hdr.magic);
        fclose(f);
        return ESP_ERR_INVALID_ARG;
    }
    if (hdr.abi != FXE_ABI) {
        ESP_LOGW(TAG, "%s：abi=%u（本版只收 %u）", name, (unsigned)hdr.abi, FXE_ABI);
        fclose(f);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (hdr.payload_off < FXE_HDR_SIZE || hdr.payload_size == 0 ||
        hdr.payload_off + hdr.payload_size > hdr.total_size) {
        ESP_LOGW(TAG, "%s：檔頭長度不對（off=%u size=%u total=%u）", name,
                 (unsigned)hdr.payload_off, (unsigned)hdr.payload_size,
                 (unsigned)hdr.total_size);
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    // ── 兩種格式：payload 是 wasm 還是 aot ──
    const bool is_aot = (hdr.flags & FXE_FLAG_AOT) != 0;
    const int arch = nc_fx_arch_target();
    if (is_aot) {
        if (hdr.target_id == NC_FX_TARGET_WASM) {
            ESP_LOGW(TAG, "%s：標成 AOT 但 target_id=0（應是 1/2）", name);
            fclose(f);
            return ESP_ERR_INVALID_ARG;
        }
        if (arch == 0 || hdr.target_id != (uint32_t)arch) {
            // 這一步是為了把「illegal instruction」變成一句看得懂的拒絕
            ESP_LOGW(TAG, "%s：AOT 是 target %u，這顆晶片是 %d → 拒絕（不會載入錯機器碼）",
                     name, (unsigned)hdr.target_id, arch);
            fclose(f);
            return ESP_ERR_NOT_SUPPORTED;
        }
    } else if (hdr.target_id != NC_FX_TARGET_WASM) {
        ESP_LOGW(TAG, "%s：沒標 AOT 卻寫 target_id=%u", name, (unsigned)hdr.target_id);
        fclose(f);
        return ESP_ERR_INVALID_ARG;
    }

    if (hdr.mem_bytes > s_arena_bytes) {
        ESP_LOGW(TAG, "%s：mem_bytes=%u 超過 arena %u", name,
                 (unsigned)hdr.mem_bytes, (unsigned)s_arena_bytes);
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    uint8_t *dst = arena_alloc(hdr.payload_size, 8);
    if (dst == NULL) {
        ESP_LOGW(TAG, "%s：arena 不夠（要 %u，剩 %u）", name,
                 (unsigned)hdr.payload_size, (unsigned)(s_arena_bytes - s_arena_used));
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    if (fseek(f, (long)hdr.payload_off, SEEK_SET) != 0 ||
        fread(dst, 1, hdr.payload_size, f) != hdr.payload_size) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    fclose(f);

    const uint32_t crc = esp_rom_crc32_le(0, dst, hdr.payload_size);
    if (crc != hdr.payload_crc) {
        ESP_LOGW(TAG, "%s：crc 不符（got %08x want %08x）", name,
                 (unsigned)crc, (unsigned)hdr.payload_crc);
        return ESP_ERR_INVALID_CRC;
    }

    // ── 登記實例（不是服務）──
    memset(slot, 0, sizeof(*slot));
    snprintf(slot->name, sizeof(slot->name), "%s", name);
    snprintf(slot->path, sizeof(slot->path), "%s", path);
    slot->payload = dst;
    slot->payload_size = hdr.payload_size;
    slot->mem_bytes = hdr.mem_bytes;
    slot->px = hdr.px;
    slot->fps = hdr.fps;
    slot->flags = hdr.flags;
    slot->fmt = is_aot ? NC_FX_FMT_AOT : NC_FX_FMT_WASM;
    slot->used = true;
    slot->ok = (s_backend != NULL);        // 沒有執行引擎時「載入成功但不能跑」

    ESP_LOGI(TAG, "載入 %s（%s %u B, mem %u B, px %u, fps %u, target=%u, 可執行=%d）",
             name, is_aot ? "aot" : "wasm", (unsigned)hdr.payload_size,
             (unsigned)hdr.mem_bytes, (unsigned)hdr.px, (unsigned)hdr.fps,
             (unsigned)hdr.target_id, slot->ok ? 1 : 0);

    if (s_live == NULL) {
        nc_fx_select(name);                // 第一個自動上線
    }
    return ESP_OK;
}

int nc_fx_scan(const char *dir)
{
    if (s_arena == NULL || dir == NULL) {
        return 0;
    }
    DIR *d = opendir(dir);
    if (d == NULL) {
        ESP_LOGW(TAG, "沒有 %s（不啟用效果）", dir);
        return 0;
    }
    int ok = 0, bad = 0;
    struct dirent *e;
    char path[NC_FX_PATH_MAX];
    while ((e = readdir(d)) != NULL) {
        const size_t n = strlen(e->d_name);
        if (n < 5 || strcmp(e->d_name + n - 4, ".fxe") != 0) {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (nc_fx_load(path) == ESP_OK) {
            ok++;
        } else {
            bad++;                          // 壞掉的不刪，留給人看
        }
    }
    closedir(d);
    ESP_LOGI(TAG, "掃描 %s：%d 個可用、%d 個被拒", dir, ok, bad);
    return ok;
}

esp_err_t nc_fx_select(const char *name)
{
    if (name == NULL) {
        s_live = NULL;
        ESP_LOGI(TAG, "停止播放");
        return ESP_OK;
    }
    nc_fx_t *m = find_mod(name);
    if (m == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (m->bad) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_live != NULL && s_live != m) {
        // 兩幀後才真的放手（文件 §5：卸載要等兩幀）
        snprintf(s_retire, sizeof(s_retire), "%s", s_live->name);
        s_retire_at = s_frame + 2;
    }
    s_live = m;
    ESP_LOGI(TAG, "live → %s%s", name, m->ok ? "" : "（無執行引擎，只驗證）");
    return ESP_OK;
}

esp_err_t nc_fx_unload(const char *name)
{
    nc_fx_t *m = find_mod(name);
    if (m == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (s_live == m) {
        nc_fx_select(NULL);
    }
    ESP_LOGI(TAG, "卸載 %s", name);
    memset(m, 0, sizeof(*m));
    arena_reset_if_empty();
    return ESP_OK;
}

uint8_t nc_fx_module_format(const nc_fx_t *fx)
{
    return fx ? fx->fmt : NC_FX_FMT_WASM;
}

nc_fx_t *nc_fx_live(void)
{
    return s_live;
}

uint32_t nc_fx_frame_id(void)
{
    return s_frame;
}

// ───────────────────────── 一幀 ─────────────────────────

// 一幀的工作：先處理退休、再 render、量預算。
// 抽出來是為了讓 host 測試能確定性地驅動（不需要 FreeRTOS）。
uint32_t nc_fx_tick(void)
{
    s_frame++;

    if (s_retire[0] != '\0' && s_frame >= s_retire_at) {
        nc_fx_t *m = find_mod(s_retire);
        if (m != NULL && m != s_live) {
            ESP_LOGI(TAG, "兩幀已過，釋放 %s", s_retire);
            memset(m, 0, sizeof(*m));
            arena_reset_if_empty();
        }
        s_retire[0] = '\0';
    }

    nc_fx_t *fx = s_live;
    if (fx == NULL || !fx->ok || s_backend == NULL || s_backend->render == NULL) {
        return s_frame;
    }

    const int64_t t0 = fx_now_us();
    const esp_err_t err = s_backend->render(fx, s_frame);
    const int64_t dt = fx_now_us() - t0;

    if (err != ESP_OK) {
        // trap / 模組自報錯誤：立刻停用，但不影響核心
        ESP_LOGE(TAG, "%s render 失敗（%s）→ 標記 bad", fx->name, esp_err_to_name(err));
        fx->bad = true;
        fx->ok = false;
        s_live = NULL;
        return s_frame;
    }

    if (dt > NC_FX_FRAME_BUDGET_US) {
        if (++fx->strikes >= NC_FX_BUDGET_STRIKES) {
            ESP_LOGE(TAG, "%s 連續 %u 次超支（%lld µs）→ 停用",
                     fx->name, (unsigned)fx->strikes, (long long)dt);
            fx->bad = true;
            fx->ok = false;
            s_live = NULL;
        } else {
            ESP_LOGW(TAG, "%s 超支 %lld µs（strike %u/%u）", fx->name,
                     (long long)dt, (unsigned)fx->strikes, (unsigned)NC_FX_BUDGET_STRIKES);
        }
    } else {
        fx->strikes = 0;
    }
    return s_frame;
}

#ifndef NC_FX_HOST_TEST
void nc_fx_task(void *arg)
{
    (void)arg;
    const TickType_t period = pdMS_TO_TICKS(10);   // 100 Hz 佔位；之後由 FrameClock 驅動
    TickType_t last = xTaskGetTickCount();

    for (;;) {
        nc_fx_tick();
        vTaskDelayUntil(&last, period);
    }
}
#endif

// ───────────────────────── 狀態 ─────────────────────────

size_t nc_fx_status_json(char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) {
        return 0;
    }
    size_t off = 0;
    off += snprintf(buf + off, cap - off,
                    "\"fx\":{\"arena\":%u,\"used\":%u,\"live\":\"%s\",\"frames\":%u,"
                    "\"backend\":\"%s\",\"modules\":[",
                    (unsigned)s_arena_bytes, (unsigned)s_arena_used,
                    s_live ? s_live->name : "",
                    (unsigned)s_frame, s_backend ? s_backend->name : "none");

    bool first = true;
    for (int i = 0; i < NC_FX_MAX_MODULES && off < cap; i++) {
        if (!s_mods[i].used) {
            continue;
        }
        off += snprintf(buf + off, cap - off,
                        "%s{\"name\":\"%s\",\"fmt\":\"%s\",\"bytes\":%u,\"ok\":%d,\"bad\":%d,\"strikes\":%u}",
                        first ? "" : ",", s_mods[i].name,
                        s_mods[i].fmt == NC_FX_FMT_AOT ? "aot" : "wasm",
                        (unsigned)s_mods[i].payload_size, s_mods[i].ok ? 1 : 0,
                        s_mods[i].bad ? 1 : 0, (unsigned)s_mods[i].strikes);
        first = false;
    }
    off += snprintf(buf + off, cap - off, "]}");
    return off;
}
