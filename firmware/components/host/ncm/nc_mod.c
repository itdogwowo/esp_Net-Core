#include "nc_mod.h"
#include "nc_mod_abi.h"
#include "nc_plugin.h"
#include "nc_ncd.h"
#include "nc_res.h"

#include <dirent.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>

#ifdef NC_MOD_HOST_TEST
// ── host 測試（Mac 上 cc 直接編）：平台東西全部換成 shim ──
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "mod_host.h"          // mod_host_now_us()
#define mod_now_us() mod_host_now_us()
#else
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "nc_cfg.h"
#include "nc_task.h"
#define mod_now_us() esp_timer_get_time()
#endif

static const char *TAG = "ncm";

// manifest 的 kind 數值直接對上 nc_task_kind_t（兩個 header 各自獨立，靠這裡釘住）。
// 舊說法的 service / task 都是 NORMAL —— 兩者是同一件事。
_Static_assert(NC_NCD_KIND_TASK == 1 && NC_NCD_KIND_EFFECT == 2,
               "manifest kind values are a wire format");
#ifndef NC_MOD_HOST_TEST
_Static_assert((int)NC_TASK_NORMAL == (int)NC_NCD_KIND_TASK, "kind mismatch");
_Static_assert((int)NC_TASK_EFFECT == (int)NC_NCD_KIND_EFFECT, "kind mismatch");
#endif

// 這顆晶片吃的 AOT target。0 = 未知（這種情況只收 wasm）。
#ifdef NC_MOD_HOST_TEST
int mod_host_arch_target = NC_MOD_TARGET_XTENSA;   // 測試可改
int nc_mod_arch_target(void) { return mod_host_arch_target; }
#else
int nc_mod_arch_target(void)
{
#if CONFIG_IDF_TARGET_ESP32P4
    return NC_MOD_TARGET_RV32F;      // P4 = RISC-V，且只有單精度 F（不能 ilp32d）
#elif CONFIG_IDF_TARGET_ESP32S3
    return NC_MOD_TARGET_XTENSA;
#else
    return 0;
#endif
}
#endif

// ── .nct / .nce 檔頭（48 B，見 ncm-loader.md §3）──
#define NCM_MAGIC        0x314D434Eu      // 'N','C','M','1' little-endian
#define NCM_HDR_SIZE     48
#define NCM_ABI          0
#define NCM_FLAG_AOT     0x0001
#define NCM_FLAG_MANIFEST 0x0002

#if defined(_MSC_VER)
#pragma pack(push, 1)
#endif
typedef struct
#ifdef __GNUC__
__attribute__((packed))
#endif
{
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
} nc_ncm_hdr_t;
#if defined(_MSC_VER)
#pragma pack(pop)
#endif

_Static_assert(sizeof(nc_ncm_hdr_t) == NCM_HDR_SIZE, "ncm header must stay 48 B");
_Static_assert(sizeof(nc_mod_desc_t) == 28, "ncm descriptor must stay 28 B");
_Static_assert(NC_MOD_PARAM_OFF == 32 && NC_MOD_DATA_OFF == 96, "ncm layout is pinned by abi v0");

#define MOD_DEFAULT_PERIOD_MS 10

struct nc_mod {
    char     name[NC_MOD_NAME_MAX];
    char     path[NC_MOD_PATH_MAX];
    uint8_t *payload;          // 指向 arena 內
    size_t   payload_size;
    size_t   arena_off;        // 這塊 payload 在 arena 的位移（卸載時回收）
    uint32_t mem_bytes;
    uint32_t px;
    uint32_t fps;
    int      level;            // 合併後的 level（-1 = 停用；attach 時用）
    uint16_t flags;
    uint8_t  fmt;              // NC_MOD_FMT_WASM / NC_MOD_FMT_AOT
    nc_plugin_t plugin;        // 插件工具的實例把手（NULL = 冇開／冇引擎）
    bool     used;
    bool     ok;               // 驗證通過、且有執行引擎
    bool     bad;              // trap 或連續超支 → 不再選用
    bool     attached;         // 已登記到 module registry（裝置端才有）
    uint8_t  strikes;          // 連續超支次數

    nc_mod_decl_t decl;                   // 解碼後的 manifest（沒 manifest 就是預設值）
    nc_meta_t     meta;                   // 自我描述（name / desc / requires / provides）
    uint16_t     cmds[NC_MOD_MAX_CMDS];
    uint8_t      ncmds;

#ifndef NC_MOD_HOST_TEST
    nc_task_t task;           // 載入期間登記的租戶
#endif
};

// ── 狀態 ──
static uint8_t *s_arena;
static size_t   s_arena_bytes;
static size_t   s_arena_used;
static nc_mod_t  s_mods[NC_MOD_MAX_MODULES];
static nc_mod_t *volatile s_live;
static volatile uint32_t s_frame;
static char     s_retire[NC_MOD_NAME_MAX];   // 等兩幀才釋放的模組
static uint32_t s_retire_at;
static nc_mod_tick_hook_t s_tick_hook;
static int      s_task_core = 1;
static int      s_task_prio = 3;

static const char *mod_kind_name(uint8_t kind);

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

static nc_mod_t *find_mod(const char *name)
{
    for (int i = 0; i < NC_MOD_MAX_MODULES; i++) {
        if (s_mods[i].used && strcmp(s_mods[i].name, name) == 0) {
            return &s_mods[i];
        }
    }
    return NULL;
}

// bump allocator：沒有逐塊 free，卸載時把「頂端」退回去（arena_compact）。
// 回收規則是安全的那一種：只把 used 降回「還活著的模組的最大尾端」，
// 不會把別人的記憶體蓋掉，也不會因為留下小洞而拒絕之後的載入。
static void *arena_alloc(size_t bytes, size_t align, size_t *off_out)
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
    if (off_out != NULL) {
        *off_out = off;
    }
    return s_arena + off;
}

static void arena_compact(void)
{
    size_t top = 0;
    for (int i = 0; i < NC_MOD_MAX_MODULES; i++) {
        if (s_mods[i].used) {
            const size_t end = s_mods[i].arena_off + s_mods[i].payload_size;
            if (end > top) {
                top = end;
            }
        }
    }
    if (top != s_arena_used) {
        s_arena_used = top;
    }
}

// 真的放掉一個實例：先拆租戶（指令/任務），再清表。
// 拆不掉就不要清 —— 模組宣告還活在這個 slot 裡，清了會是 use-after-free。
static esp_err_t release_slot(nc_mod_t *m)
{
    // 先收引擎那邊的資源（instance／module／exec env），之後才清 loader 的資料。
    if (m->plugin != NULL) {
        nc_plugin_close(m->plugin);
        m->plugin = NULL;
    }
#ifndef NC_MOD_HOST_TEST
    if (m->attached) {
        const esp_err_t e = nc_task_detach(m->name);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "%s：detach 失敗（%s）→ 保留實例，不釋放", m->name,
                     esp_err_to_name(e));
            m->bad = true;
            m->ok = false;
            return e;
        }
        m->attached = false;
    }
#endif
    memset(m, 0, sizeof(*m));
    arena_compact();
    return ESP_OK;
}

// ───────────────────────── 生命週期 ─────────────────────────

esp_err_t nc_mod_init(void *arena, size_t bytes)
{
    s_arena = (uint8_t *)arena;
    s_arena_bytes = bytes;
    s_arena_used = 0;
    s_live = NULL;
    s_frame = 0;
    s_retire[0] = '\0';
    memset(s_mods, 0, sizeof(s_mods));
    nc_plugin_set_arena(arena, bytes);     // 插件實例的 payload 住喺這塊 arena

    if (s_arena == NULL) {
        ESP_LOGW(TAG, "拿不到 arena（plan 沒給）→ ncm 停用");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "arena %u B（backend=%s, task core=%d prio=%d）",
             (unsigned)bytes, nc_plugin_backend_name(),
             s_task_core, s_task_prio);
    return ESP_OK;
}

void nc_mod_set_task_placement(int core, int prio)
{
    s_task_core = core;
    s_task_prio = prio;
}

int nc_mod_task_core(void)
{
    return s_task_core;
}

// ───────────────────────── 載入 / 卸載 ─────────────────────────

// 副檔名只是「人看的分類標籤」：.nct = 任務、.nce = 效果。
// 真正的 kind 以 manifest 為準；沒有 manifest 才用副檔名當預設。
static uint8_t ext_kind_of(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (dot != NULL && strcmp(dot, ".nct") == 0) {
        return NC_NCD_KIND_TASK;
    }
    return NC_NCD_KIND_EFFECT;      // .nce 或未知 → effect
}

// 把 manifest（若有）解碼成 decl；沒有 manifest 就給「純效果」的預設值。
// 只讀檔頭 + manifest + 指令表，不碰 payload —— 掃描階段（nc_mod_probe）也用這支。
static esp_err_t read_manifest(FILE *f, const nc_ncm_hdr_t *h, const char *path,
                               nc_mod_decl_t *decl, uint16_t *cmds, uint8_t *ncmds,
                               nc_meta_t *meta)
{
    if (meta != NULL) {
        memset(meta, 0, sizeof(*meta));
    }
    const uint8_t ext_kind = ext_kind_of(path);
    *decl = (nc_mod_decl_t){
        .has_manifest = false,
        .kind = ext_kind,
        .core = -1,
        .layer = 3,
        .prio = 3,
        .stack = 4096,
        .fps = h->fps,
        .px = h->px,
        .budget_us = NC_MOD_FRAME_BUDGET_US,
        .mem_bytes = h->mem_bytes,
    };
    *ncmds = 0;

    if ((h->flags & NCM_FLAG_MANIFEST) == 0) {
        return ESP_OK;
    }
    if (h->manifest_off < NCM_HDR_SIZE || h->manifest_size < NC_NCD_HDR_SIZE ||
        (uint64_t)h->manifest_off + h->manifest_size > h->total_size) {
        ESP_LOGW(TAG, "%s：manifest 區塊長度不對（off=%u size=%u total=%u）", path,
                 (unsigned)h->manifest_off, (unsigned)h->manifest_size,
                 (unsigned)h->total_size);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t raw[NC_NCD_HDR_SIZE];
    if (fseek(f, (long)h->manifest_off, SEEK_SET) != 0 ||
        fread(raw, 1, sizeof(raw), f) != sizeof(raw)) {
        return ESP_ERR_INVALID_SIZE;
    }

    nc_ncd_t m;
    const nc_ncd_err_t me = nc_ncd_parse(raw, sizeof(raw), h->total_size, &m);
    if (me != NC_NCD_OK) {
        ESP_LOGW(TAG, "%s：manifest 壞（%s）", path, nc_ncd_err_name(me));
        return ESP_ERR_INVALID_ARG;
    }

    // 一份真相：manifest 與檔頭講同一件事時必須一致，不一致 = 工具鏈壞了。
    if (m.px != 0 && m.px != h->px) {
        ESP_LOGW(TAG, "%s：manifest px=%u 但檔頭 px=%u", path,
                 (unsigned)m.px, (unsigned)h->px);
        return ESP_ERR_INVALID_ARG;
    }
    if (m.mem_bytes != 0 && m.mem_bytes != h->mem_bytes) {
        ESP_LOGW(TAG, "%s：manifest mem=%u 但檔頭 mem=%u", path,
                 (unsigned)m.mem_bytes, (unsigned)h->mem_bytes);
        return ESP_ERR_INVALID_ARG;
    }
    if (m.fps != 0 && m.fps != h->fps) {
        ESP_LOGW(TAG, "%s：manifest fps=%u 但檔頭 fps=%u", path,
                 (unsigned)m.fps, (unsigned)h->fps);
        return ESP_ERR_INVALID_ARG;
    }

    // 副檔名與 manifest 衝突：manifest 為準（副檔名只是人看的標籤）。
    if (m.kind != ext_kind) {
        ESP_LOGW(TAG, "%s：副檔名說 kind=%s，manifest 說 %s → 以 manifest 為準",
                 path, mod_kind_name(ext_kind), mod_kind_name(m.kind));
    }

    decl->has_manifest = true;
    decl->kind = m.kind;
    decl->core = m.core;
    decl->layer = m.layer;
    decl->prio = m.prio;
    decl->stack = m.stack ? m.stack : 4096;
    decl->fps = m.fps ? m.fps : h->fps;
    decl->px = m.px ? m.px : h->px;
    decl->budget_us = m.budget_us ? m.budget_us : NC_MOD_FRAME_BUDGET_US;
    decl->mem_bytes = m.mem_bytes ? m.mem_bytes : h->mem_bytes;
    decl->ncmds = m.ncmds;

    // meta 區塊（可選）：name / desc / usage / requires / provides
    if (meta != NULL && m.meta_off != 0 && m.meta_size >= NC_META_SIZE &&
        (uint64_t)m.meta_off + m.meta_size <= h->total_size) {
        uint8_t raw_meta[NC_META_SIZE];
        if (fseek(f, (long)m.meta_off, SEEK_SET) == 0 &&
            fread(raw_meta, 1, sizeof(raw_meta), f) == sizeof(raw_meta)) {
            (void)nc_meta_parse(raw_meta, sizeof(raw_meta), meta);
        }
    }

    if (m.ncmds > NC_MOD_MAX_CMDS) {
        ESP_LOGW(TAG, "%s：manifest 宣告 %u 條指令，本版最多 %d 條", path,
                 (unsigned)m.ncmds, NC_MOD_MAX_CMDS);
        return ESP_ERR_NOT_SUPPORTED;
    }

    // 指令表：讀進實例（之後 WAMR 接上時直接綁 export）
    for (int i = 0; i < m.ncmds && i < NC_MOD_MAX_CMDS; i++) {
        uint8_t two[2];
        if (fseek(f, (long)(m.cmds_off + (uint32_t)i * 2u), SEEK_SET) != 0 ||
            fread(two, 1, 2, f) != 2) {
            return ESP_ERR_INVALID_SIZE;
        }
        const uint16_t cmd = (uint16_t)(two[0] | (two[1] << 8));
        for (int k = 0; k < *ncmds; k++) {
            if (cmds[k] == cmd) {
                ESP_LOGW(TAG, "%s：manifest 裡 cmd 0x%04X 重複", path, (unsigned)cmd);
                return ESP_ERR_INVALID_ARG;
            }
        }
        cmds[(*ncmds)++] = cmd;
    }
    decl->ncmds = *ncmds;
    return ESP_OK;
}

// 把解碼結果變成一個任務，登記到任務管理器。
static esp_err_t attach_module(nc_mod_t *slot)
{
#ifndef NC_MOD_HOST_TEST
    slot->task = (nc_task_t){
        .name = slot->name,
        .kind = (nc_task_kind_t)slot->decl.kind,
        .level = slot->level,   // 合併後的 level（-1 = 停用；attach 是 runtime）
        .mem = NULL, .nmem = 0, // 記憶體來自 ncm.arena，不進 plan
        .claims = NULL, .nclaims = 0,
        .provides = NULL, .nprovides = 0,
        .loops = NULL, .nloops = 0,
        .declare = NULL,
        .on_start = NULL,
        .on_stop = NULL,
    };
    const esp_err_t e = nc_task_attach(&slot->task);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "%s：登記任務失敗 %s", slot->name, esp_err_to_name(e));
        return e;
    }
    slot->attached = true;
#else
    (void)slot;                            // host 測試沒有 registry
#endif
    return ESP_OK;
}

// 檔頭驗證（載入與掃描共用）。不碰 arena —— 掃描階段沒有 arena。
static esp_err_t validate_header(const nc_ncm_hdr_t *hdr, const char *name)
{
    if (hdr->magic != NCM_MAGIC) {
        ESP_LOGW(TAG, "%s：magic 不對（0x%08x）", name, (unsigned)hdr->magic);
        return ESP_ERR_INVALID_ARG;
    }
    if (hdr->abi != NCM_ABI) {
        ESP_LOGW(TAG, "%s：abi=%u（本版只收 %u）", name, (unsigned)hdr->abi, NCM_ABI);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (hdr->payload_off < NCM_HDR_SIZE || hdr->payload_size == 0 ||
        hdr->payload_off + hdr->payload_size > hdr->total_size) {
        ESP_LOGW(TAG, "%s：檔頭長度不對（off=%u size=%u total=%u）", name,
                 (unsigned)hdr->payload_off, (unsigned)hdr->payload_size,
                 (unsigned)hdr->total_size);
        return ESP_ERR_INVALID_SIZE;
    }
    // ── 兩種格式：payload 是 wasm 還是 aot ──
    const bool is_aot = (hdr->flags & NCM_FLAG_AOT) != 0;
    const int arch = nc_mod_arch_target();
    if (is_aot) {
        if (hdr->target_id == NC_MOD_TARGET_WASM) {
            ESP_LOGW(TAG, "%s：標成 AOT 但 target_id=0（應是 1/2）", name);
            return ESP_ERR_INVALID_ARG;
        }
        if (arch == 0 || hdr->target_id != (uint32_t)arch) {
            // 這一步是為了把「illegal instruction」變成一句看得懂的拒絕
            ESP_LOGW(TAG, "%s：AOT 是 target %u，這顆晶片是 %d → 拒絕（不會載入錯機器碼）",
                     name, (unsigned)hdr->target_id, arch);
            return ESP_ERR_NOT_SUPPORTED;
        }
    } else if (hdr->target_id != NC_MOD_TARGET_WASM) {
        ESP_LOGW(TAG, "%s：沒標 AOT 卻寫 target_id=%u", name, (unsigned)hdr->target_id);
        return ESP_ERR_INVALID_ARG;
    }
    if (hdr->mem_bytes > NC_MOD_MAX_LINEAR) {
        ESP_LOGW(TAG, "%s：mem_bytes=%u 超過線性記憶體上限 %u", name,
                 (unsigned)hdr->mem_bytes, (unsigned)NC_MOD_MAX_LINEAR);
        return ESP_ERR_INVALID_SIZE;
    }
#if NC_MOD_MAX_PIXELS < 65535u
    if (hdr->px > NC_MOD_MAX_PIXELS) {
        ESP_LOGW(TAG, "%s：px=%u 超過上限 %u", name, (unsigned)hdr->px,
                 (unsigned)NC_MOD_MAX_PIXELS);
        return ESP_ERR_INVALID_SIZE;
    }
#endif
    return ESP_OK;
}

esp_err_t nc_mod_load_ex(const char *path, int level)
{
    if (s_arena == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char name[NC_MOD_NAME_MAX];
    base_name(path, name, sizeof(name));
    if (name[0] == '\0' || strcmp(name, "ncm") == 0) {
        return ESP_ERR_INVALID_ARG;        // "ncm" 是政策模組自己的名字
    }
    if (find_mod(name) != NULL) {
        return ESP_ERR_INVALID_STATE;      // 已載入
    }

    nc_mod_t *slot = NULL;
    for (int i = 0; i < NC_MOD_MAX_MODULES; i++) {
        if (!s_mods[i].used) {
            slot = &s_mods[i];
            break;
        }
    }
    if (slot == NULL) {
        ESP_LOGW(TAG, "實例表滿了（%d）", NC_MOD_MAX_MODULES);
        return ESP_ERR_NO_MEM;
    }
    const size_t arena_mark = s_arena_used;

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGW(TAG, "開不了 %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    nc_ncm_hdr_t hdr;
    if (fread(&hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }

    // ── 驗證（六步的前三步；與 nc_mod_probe 共用）──
    const esp_err_t verr = validate_header(&hdr, name);
    if (verr != ESP_OK) {
        fclose(f);
        return verr;
    }
    const bool is_aot = (hdr.flags & NCM_FLAG_AOT) != 0;

    // ── 上限：超過就拒絕載入，不是中途撇除（abi v0 的契約）──
    if (hdr.mem_bytes > s_arena_bytes) {
        ESP_LOGW(TAG, "%s：mem_bytes=%u 超過 arena %u", name,
                 (unsigned)hdr.mem_bytes, (unsigned)s_arena_bytes);
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    // ── manifest：模組自我宣告（類型 / 核心 / 記憶體 / 節奏）──
    memset(slot, 0, sizeof(*slot));
    snprintf(slot->name, sizeof(slot->name), "%s", name);
    snprintf(slot->path, sizeof(slot->path), "%s", path);
    const esp_err_t merr = read_manifest(f, &hdr, path, &slot->decl, slot->cmds,
                                         &slot->ncmds, &slot->meta);
    if (merr != ESP_OK) {
        memset(slot, 0, sizeof(*slot));
        fclose(f);
        return merr;
    }

    // 效果只能跑在 ncm task 那一核；manifest 寫別核 = 工具鏈設定錯，載入前就擋。
    // 任務類（kind = normal）唔受此限：跑哪一核由 tasks.json 決定。
    if (slot->decl.kind == NC_NCD_KIND_EFFECT && slot->decl.core != -1 &&
        slot->decl.core != s_task_core) {
        ESP_LOGW(TAG, "%s：宣告 core=%d 但 ncm task 在 core %d → 拒絕（先改排程）",
                 name, slot->decl.core, s_task_core);
        memset(slot, 0, sizeof(*slot));
        fclose(f);
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint8_t *dst = arena_alloc(hdr.payload_size, 8, &slot->arena_off);
    if (dst == NULL) {
        ESP_LOGW(TAG, "%s：arena 不夠（要 %u，剩 %u）", name,
                 (unsigned)hdr.payload_size, (unsigned)(s_arena_bytes - s_arena_used));
        memset(slot, 0, sizeof(*slot));
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    if (fseek(f, (long)hdr.payload_off, SEEK_SET) != 0 ||
        fread(dst, 1, hdr.payload_size, f) != hdr.payload_size) {
        s_arena_used = arena_mark;         // 讀失敗 → 把剛剛要的那段還回去
        memset(slot, 0, sizeof(*slot));
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    fclose(f);

    const uint32_t crc = esp_rom_crc32_le(0, dst, hdr.payload_size);
    if (crc != hdr.payload_crc) {
        ESP_LOGW(TAG, "%s：crc 不符（got %08x want %08x）", name,
                 (unsigned)crc, (unsigned)hdr.payload_crc);
        s_arena_used = arena_mark;         // 壞檔不留半塊 arena（原本會漏）
        memset(slot, 0, sizeof(*slot));
        return ESP_ERR_INVALID_CRC;
    }

    // ── 登記實例（不是服務，是租戶）──
    slot->payload = dst;
    slot->payload_size = hdr.payload_size;
    slot->mem_bytes = slot->decl.mem_bytes;
    slot->px = slot->decl.px;
    slot->fps = slot->decl.fps;
    slot->level = level;
    slot->flags = hdr.flags;
    slot->fmt = is_aot ? NC_MOD_FMT_AOT : NC_MOD_FMT_WASM;
    slot->used = true;
    slot->ok = nc_plugin_ready();          // 沒有執行引擎時「載入成功但不能跑」
    slot->plugin = NULL;

    // 有引擎就即刻實例化（payload／CRC 已驗）。失敗唔踢走：留在表裡標 bad，
    // 主機可以由 status 看到「呢個模組存在但開唔成」，唔會靜靜消失。
    if (slot->ok) {
        nc_plugin_src_t src = {
            .name = slot->name,
            .payload = slot->payload,
            .size = slot->payload_size,
            .fmt = slot->fmt,
            .kind = slot->decl.kind,
            .px = (uint16_t)slot->decl.px,
            .mem_bytes = slot->decl.mem_bytes,
        };
        const esp_err_t lerr = nc_plugin_open(&slot->plugin, &src);
        if (lerr != ESP_OK) {
            ESP_LOGE(TAG, "%s：插件開唔成（%s）→ 標記 bad（唔會跑）",
                     name, esp_err_to_name(lerr));
            slot->bad = true;
            slot->ok = false;
        }
    }

    const esp_err_t aerr = attach_module(slot);
    if (aerr != ESP_OK) {
        s_arena_used = arena_mark;
        memset(slot, 0, sizeof(*slot));
        return aerr;
    }

    ESP_LOGI(TAG, "載入 %s（%s %u B, mem %u B, px %u, fps %u, target=%u, kind=%s, core=%d, 可執行=%d）",
             name, is_aot ? "aot" : "wasm", (unsigned)hdr.payload_size,
             (unsigned)slot->mem_bytes, (unsigned)slot->px, (unsigned)slot->fps,
             (unsigned)hdr.target_id,
             mod_kind_name(slot->decl.kind),
             slot->decl.core, slot->ok ? 1 : 0);

    if (s_live == NULL && slot->ok && !slot->bad) {
        nc_mod_select(name);                // 第一個「跑得動」的自動上線
    }
    return ESP_OK;
}

// 舊介面：level 固定 5（模組在一般任務之後）。
esp_err_t nc_mod_load(const char *path)
{
    return nc_mod_load_ex(path, 5);
}

// 掃描階段：只讀檔頭 + manifest + 指令表，不碰 payload / arena。
// 這是「先掃描、再讀 tasks.json、合併、最後啟動」的那個掃描。
esp_err_t nc_mod_probe(const char *path, nc_mod_probe_t *out)
{
    if (path == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    nc_ncm_hdr_t hdr;
    if (fread(&hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    char name[NC_MOD_NAME_MAX];
    base_name(path, name, sizeof(name));
    const esp_err_t verr = validate_header(&hdr, name);
    if (verr != ESP_OK) {
        fclose(f);
        return verr;
    }

    nc_mod_decl_t decl;
    uint16_t cmds[NC_MOD_MAX_CMDS];
    uint8_t ncmds = 0;
    nc_meta_t meta;
    const esp_err_t merr = read_manifest(f, &hdr, path, &decl, cmds, &ncmds, &meta);
    fclose(f);
    if (merr != ESP_OK) {
        return merr;
    }

    memset(out, 0, sizeof(*out));
    snprintf(out->name, sizeof(out->name), "%s", name);
    out->fmt = (hdr.flags & NCM_FLAG_AOT) ? NC_MOD_FMT_AOT : NC_MOD_FMT_WASM;
    out->target_id = hdr.target_id;
    out->decl = decl;
    out->meta = meta;
    for (int i = 0; i < ncmds; i++) {
        out->cmds[i] = cmds[i];
    }
    return ESP_OK;
}

int nc_mod_scan(const char *dir)
{
    if (s_arena == NULL || dir == NULL) {
        return 0;
    }
    DIR *d = opendir(dir);
    if (d == NULL) {
        ESP_LOGW(TAG, "沒有 %s（不啟用模組）", dir);
        return 0;
    }
    int ok = 0, bad = 0;
    struct dirent *e;
    char path[NC_MOD_PATH_MAX];
    while ((e = readdir(d)) != NULL) {
        const char *ext = strrchr(e->d_name, '.');
        if (ext == NULL || (strcmp(ext, ".nct") != 0 && strcmp(ext, ".nce") != 0)) {
            continue;
        }
        /* 砌路徑要**先確認放得落**：d_name 最長 255，加上 dir 隨時超過 80。
         * 照截斷去開檔係最壞的情況 —— 會靜靜開到另一個（前綴相同的）檔。
         * 所以寧願當「被拒」，計入 bad，上面個 log 會報數。 */
        const int m = snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (m < 0 || (size_t)m >= sizeof(path)) {
            ESP_LOGW(TAG, "路徑太長，跳過：%s/%s", dir, e->d_name);
            bad++;
            continue;
        }
        if (nc_mod_load(path) == ESP_OK) {
            ok++;
        } else {
            bad++;                          // 壞掉的不刪，留給人看
        }
    }
    closedir(d);
    ESP_LOGI(TAG, "掃描 %s：%d 個可用、%d 個被拒", dir, ok, bad);
    return ok;
}

// ───────────────────────── 開機框架：掃描 → 合併 → 載入 ─────────────────────────

int nc_mod_scan_dir(const char *dir, nc_mod_scan_t *out)
{
    if (dir == NULL || out == NULL) {
        return 0;
    }
    DIR *d = opendir(dir);
    if (d == NULL) {
        ESP_LOGW(TAG, "沒有 %s（不啟用模組）", dir);
        return 0;
    }
    int ok = 0, bad = 0;
    struct dirent *e;
    char path[NC_MOD_PATH_MAX];
    while ((e = readdir(d)) != NULL) {
        const char *ext = strrchr(e->d_name, '.');
        if (ext == NULL || (strcmp(ext, ".nct") != 0 && strcmp(ext, ".nce") != 0)) {
            continue;
        }
        const int m = snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (m < 0 || (size_t)m >= sizeof(path)) {
            ESP_LOGW(TAG, "路徑太長，跳過：%s/%s", dir, e->d_name);
            bad++;
            continue;
        }
        if (out->count >= NC_MOD_MAX_ENTRIES) {
            ESP_LOGW(TAG, "掃描表滿了（%d）→ 跳過 %s", NC_MOD_MAX_ENTRIES, path);
            bad++;
            continue;
        }
        nc_mod_entry_t *ent = &out->entries[out->count];
        memset(ent, 0, sizeof(*ent));
        snprintf(ent->path, sizeof(ent->path), "%s", path);
        if (nc_mod_probe(path, &ent->probe) == ESP_OK) {
            struct stat st;
            if (stat(path, &st) == 0 && st.st_size > 0) {
                out->bytes += (size_t)st.st_size;
            }
            out->count++;
            ok++;
        } else {
            bad++;
        }
    }
    closedir(d);
    out->bad += bad;
    ESP_LOGI(TAG, "掃描 %s：%d 個可用、%d 個被拒", dir, ok, bad);
    return ok;
}

// arena 大小：掃到的檔案總大小 ＋ 每個模組 1 KB（header／manifest／對齊 slack），
// 32 B 對齊，夾在 [MIN, MAX]。冇模組 → MIN：唔再無條件預留 512 KB。
size_t nc_mod_arena_bytes(const nc_mod_scan_t *scan)
{
    // 一個模組都冇 → 0 B：arena 係「載入模組的地方」，冇模組就唔需要存在。
    // 有模組才用 MIN 做下限（WAMR 模組 metadata／對齊 headroom）。
    if (scan == NULL || scan->count <= 0) {
        return 0;
    }
    size_t need = scan->bytes;
    need += (size_t)scan->count * 1024u;
    need = (need + 31u) & ~(size_t)31u;
    if (need < NC_MOD_ARENA_MIN) {
        need = NC_MOD_ARENA_MIN;
    }
    if (need > NC_MOD_ARENA_MAX) {
        need = NC_MOD_ARENA_MAX;
    }
    return need;
}

int nc_mod_scan_all(nc_mod_scan_t *out)
{
    if (out == NULL) {
        return 0;
    }
    memset(out, 0, sizeof(*out));
    int n = 0;
    n += nc_mod_scan_dir("/data/pixel/effects", out);
    n += nc_mod_scan_dir("/data/tasks", out);
    return n;

}

int nc_mod_merge_cfg(nc_mod_scan_t *scan)
{
    if (scan == NULL) {
        return 0;
    }
    int merged = 0;
    for (int i = 0; i < scan->count; i++) {
        nc_mod_entry_t *e = &scan->entries[i];
        bool seen = false;
#ifndef NC_MOD_HOST_TEST
        const nc_tasks_cfg_t *cfg = nc_cfg_tasks();
        if (cfg != NULL) {
            for (int k = 0; k < cfg->n_tasks; k++) {
                if (strcmp(cfg->tasks[k].name, e->probe.name) == 0) {
                    e->level = cfg->tasks[k].has_level ? cfg->tasks[k].level
                                                       : e->probe.decl.layer;
                    e->aff[0] = cfg->tasks[k].aff[0] ? 1 : 0;
                    e->aff[1] = cfg->tasks[k].aff[1] ? 1 : 0;
                    seen = true;
                    break;
                }
            }
        }
#endif
        if (!seen) {
            e->level = e->probe.decl.layer;
            e->aff[0] = e->aff[1] = 0;
#ifndef NC_MOD_HOST_TEST
            char obj[160];
            snprintf(obj, sizeof(obj),
                     "{ \"name\": \"%s\", \"level\": %d, \"affinity\": [0, 0] }",
                     e->probe.name, e->level);
            if (nc_cfg_append_obj("tasks", "tasks", obj)) {
                ESP_LOGI(TAG, "cfg 補寫 %s", e->probe.name);
            }
#endif
        }
        // 模組的 level 決定「載不載入」；affinity 先存著（將來每模組一個 loop 才用）。
        e->enabled = (e->level >= 0);
        merged++;
    }
    return merged;
}

int nc_mod_load_all(const nc_mod_scan_t *scan)
{
    if (scan == NULL) {
        return 0;
    }
    int ok = 0;
    for (int i = 0; i < scan->count; i++) {
        const nc_mod_entry_t *e = &scan->entries[i];
        // `.nce`（kind=effect）唔係租戶任務：由 ncm tick 驅動，唔應該因為
        // tasks.json 冇條目（affinity=[0,0]）就被跳過。
        const bool is_effect = (e->probe.decl.kind == NC_NCD_KIND_EFFECT);
        if (!e->enabled && !is_effect) {
            ESP_LOGI(TAG, "跳過 %s（level=%d aff=(%d,%d)）", e->probe.name,
                     e->level, e->aff[0], e->aff[1]);
            continue;
        }
        char why[96];
        if (!nc_res_check(e->probe.name, why, sizeof(why))) {
            ESP_LOGW(TAG, "跳過 %s：%s", e->probe.name, why);
            continue;
        }
        if (nc_mod_load_ex(e->path, (e->level >= 0) ? e->level : 5) == ESP_OK) {
            ok++;
        }
    }
    return ok;
}

int nc_mod_declare_res(const nc_mod_scan_t *scan)
{
    if (scan == NULL) {
        return 0;
    }
    int n = 0;
    for (int i = 0; i < scan->count; i++) {
        const nc_mod_entry_t *e = &scan->entries[i];
        const char *owner = e->probe.name;
        for (int k = 0; k < e->probe.meta.n_provides; k++) {
            const nc_meta_entry_t *p = &e->probe.meta.provides[k];
            nc_res_add_prov(p->name, (nc_res_type_t)p->type, owner, p->bytes,
                            (p->flags & 1) != 0);
            n++;
        }
        for (int k = 0; k < e->probe.meta.n_requires; k++) {
            const nc_meta_entry_t *r = &e->probe.meta.requires[k];
            nc_res_add_req(r->name, (nc_res_type_t)r->type, owner, r->bytes,
                           (r->flags & 1) != 0);
            n++;
        }
    }
    return n;
}

esp_err_t nc_mod_select(const char *name)
{
    if (name == NULL) {
        s_live = NULL;
        ESP_LOGI(TAG, "停止播放");
        return ESP_OK;
    }
    nc_mod_t *m = find_mod(name);
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

esp_err_t nc_mod_unload(const char *name)
{
    nc_mod_t *m = find_mod(name);
    if (m == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (s_live == m) {
        nc_mod_select(NULL);
    }
    ESP_LOGI(TAG, "卸載 %s", name);
    return release_slot(m);
}

nc_mod_t *nc_mod_live(void)
{
    return s_live;
}

void nc_mod_set_tick_hook(nc_mod_tick_hook_t fn)
{
    s_tick_hook = fn;
}

uint32_t nc_mod_frame_id(void)
{
    return s_frame;
}

bool nc_mod_decl(const nc_mod_t *ncm, nc_mod_decl_t *out)
{
    if (ncm == NULL || out == NULL) {
        return false;
    }
    *out = ncm->decl;
    out->ncmds = ncm->ncmds;
    return true;
}

bool nc_mod_cmd_at(const nc_mod_t *ncm, int i, uint16_t *cmd)
{
    if (ncm == NULL || cmd == NULL || i < 0 || i >= ncm->ncmds) {
        return false;
    }
    *cmd = ncm->cmds[i];
    return true;
}

uint32_t nc_mod_period_ms(void)
{
    const nc_mod_t *ncm = s_live;
    if (ncm == NULL || ncm->decl.fps == 0) {
        return MOD_DEFAULT_PERIOD_MS;
    }
    uint32_t ms = 1000u / ncm->decl.fps;
    if (ms == 0) {
        ms = 1;
    }
    return ms;
}

// ───────────────────────── 一幀 ─────────────────────────

// 一幀的工作：先處理退休、再 render、量預算。
// 抽出來是為了讓 host 測試能確定性地驅動（不需要 FreeRTOS）。
uint32_t nc_mod_tick(void)
{
    s_frame++;

    if (s_retire[0] != '\0' && s_frame >= s_retire_at) {
        nc_mod_t *m = find_mod(s_retire);
        if (m != NULL && m != s_live) {
            ESP_LOGI(TAG, "兩幀已過，釋放 %s", s_retire);
            release_slot(m);
        }
        s_retire[0] = '\0';
    }

    nc_mod_t *ncm = s_live;
    if (ncm == NULL || !ncm->ok || ncm->plugin == NULL) {
        return s_frame;
    }

    const int64_t t0 = mod_now_us();
    const esp_err_t err = nc_plugin_step(ncm->plugin, s_frame);
    if (err == ESP_OK && s_tick_hook != NULL) {
        s_tick_hook(ncm->plugin);          // 例如把 .nce framebuffer 交去值流信箱
    }
    const int64_t dt = mod_now_us() - t0;

    if (err != ESP_OK) {
        // trap / 模組自報錯誤：立刻停用，但不影響核心
        ESP_LOGE(TAG, "%s render 失敗（%s）→ 標記 bad", ncm->name, esp_err_to_name(err));
        ncm->bad = true;
        ncm->ok = false;
        s_live = NULL;
        return s_frame;
    }

    // 預算：manifest 宣告的 budget_us 優先，沒宣告才用 ABI 的預設。
    const uint32_t budget = ncm->decl.budget_us ? ncm->decl.budget_us : NC_MOD_FRAME_BUDGET_US;
    if (dt > budget) {
        if (++ncm->strikes >= NC_MOD_BUDGET_STRIKES) {
            ESP_LOGE(TAG, "%s 連續 %u 次超支（%lld µs > %u）→ 停用",
                     ncm->name, (unsigned)ncm->strikes, (long long)dt, (unsigned)budget);
            ncm->bad = true;
            ncm->ok = false;
            s_live = NULL;
        } else {
            ESP_LOGW(TAG, "%s 超支 %lld µs（strike %u/%u）", ncm->name,
                     (long long)dt, (unsigned)ncm->strikes, (unsigned)NC_MOD_BUDGET_STRIKES);
        }
    } else {
        ncm->strikes = 0;
    }
    return s_frame;
}

static const char *mod_kind_name(uint8_t kind)
{
    switch (kind) {
    case NC_NCD_KIND_TASK:   return "task";
    case NC_NCD_KIND_EFFECT: return "effect";
    default:                 return "?";
    }
}

#ifndef NC_MOD_HOST_TEST
void nc_mod_task(void *arg)
{
    (void)arg;

    for (;;) {
        if (nc_task_exit_requested()) {
            ESP_LOGI(TAG, "ncm task 收到結束請求");
            break;
        }
        nc_task_beat();     // 長跑 loop：自己報心跳（停滯偵測靠它）
        nc_mod_tick();
        // 節奏由 live 模組的 manifest 決定；沒有 manifest 就 10 ms（100 Hz）。
        // 用 vTaskDelay 而不是 Until：週期會隨模組改變，Until 會累積漂移。
        vTaskDelay(pdMS_TO_TICKS(nc_mod_period_ms()));
    }
}
#endif

// ───────────────────────── 狀態 ─────────────────────────

// 有界的 append：snprintf 回的是「本來想寫多少」，off 可能超過 cap；
// 直接拿 cap - off 下去會 size_t 下溢 → 覆寫。這裡一次收好。
static size_t mod_append(char *buf, size_t cap, size_t off, const char *fmt, ...)
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

// 模組子系統的狀態「物件」（不含 "ncm": 前置）。
// 拆出來是因為它有兩種消費者：STATUS_GET（要 "ncm":{...}）與 provide
// （"ncm.info"，要的是值本身）。內容只寫一份。
size_t nc_mod_info_json(char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) {
        return 0;
    }
    size_t off = mod_append(buf, cap, 0,
                    "{\"arena\":%u,\"used\":%u,\"live\":\"%s\",\"frames\":%u,"
                    "\"backend\":\"%s\",\"period_ms\":%u,\"task_core\":%d,\"modules\":[",
                    (unsigned)s_arena_bytes, (unsigned)s_arena_used,
                    s_live ? s_live->name : "",
                    (unsigned)s_frame, nc_plugin_backend_name(),
                    (unsigned)nc_mod_period_ms(), s_task_core);

    bool first = true;
    bool cut = false;
    int n_used = 0;
    for (int i = 0; i < NC_MOD_MAX_MODULES; i++) {
        const nc_mod_t *m = &s_mods[i];
        if (!m->used) {
            continue;
        }
        n_used++;
        /* 每個模組大約 250 B。緩衝（main.c 的 part 是 1,900 B）放唔落 8 個係遲早的事，
         * 所以這裡要主動截，唔可以寫到一半就交出去 —— 半截 JSON 主機一定 parse 失敗。
         * 留 3 B 給 "]}": 唔夠就唔寫，最後走「精簡但合法」那條路。 */
        if (off + 3 > cap) {
            cut = true;
            continue;
        }
        const nc_mod_decl_t d = m->decl;
        off = mod_append(buf, cap, off,
                        "%s{\"name\":\"%s\",\"fmt\":\"%s\",\"bytes\":%u,\"ok\":%d,"
                        "\"bad\":%d,\"strikes\":%u,\"kind\":\"%s\",\"core\":%d,"
                        "\"layer\":%d,\"prio\":%d,\"fps\":%u,\"px\":%u,"
                        "\"budget_us\":%u,\"manifest\":%d,\"cmds\":%u,"
                        "\"attached\":%d}",
                        first ? "" : ",", m->name,
                        m->fmt == NC_MOD_FMT_AOT ? "aot" : "wasm",
                        (unsigned)m->payload_size, m->ok ? 1 : 0,
                        m->bad ? 1 : 0, (unsigned)m->strikes,
                        mod_kind_name(d.kind),
                        d.core, d.layer, d.prio,
                        (unsigned)d.fps, (unsigned)d.px,
                        (unsigned)d.budget_us,
                        d.has_manifest ? 1 : 0, (unsigned)d.ncmds,
                        m->attached ? 1 : 0);
        first = false;
    }
    if (cut) {
        /* 放唔落：丟掉已寫的模組，重寫一個**合法**的精簡物件。
         * "truncated":1 是給人看的訊號；"mods" 是表上實際有幾個。 */
        return mod_append(buf, cap, 0,
                         "{\"scan\":{\"n\":%d,\"bad\":%d},\"arena\":%u,\"used\":%u,\"live\":\"%s\","
                         "\"frames\":%u,\"backend\":\"%s\",\"period_ms\":%u,"
                         "\"truncated\":1,\"mods\":%d,\"modules\":[]}",
                         (unsigned)s_arena_bytes, (unsigned)s_arena_used,
                         s_live ? s_live->name : "", (unsigned)s_frame,
                         nc_plugin_backend_name(),
                         (unsigned)nc_mod_period_ms(), n_used);
    }
    return mod_append(buf, cap, off, "]}");
}

size_t nc_mod_status_json(char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) {
        return 0;
    }
    size_t off = mod_append(buf, cap, 0, "\"ncm\":");
    const size_t n = nc_mod_info_json(buf + off, cap - off);
    off += n;
    return off >= cap ? cap - 1 : off;
}
