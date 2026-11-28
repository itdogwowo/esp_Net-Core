// nc_plugin_wamr.c — 插件工具的 WAMR backend：「payload 怎麼跑」那一層。
//
// 工具分兩邊（見 nc_plugin.h）：
//   外殼 nc_plugin.c     backend 註冊、暫停閘、四個動詞轉發
//   backend 這支          WAMR runtime 能力（init／instantiate／call／回收）
//
// 這支只實作 `nc_plugin_backend_t` 的 hooks；客戶端（loader／status／task）
// 唔識 WAMR。要換引擎（AOT、其他 runtime）就換這支，上層唔動。
//
// 支援兩種 payload（同一個 .nct／.nce 容器，靠 manifest 的 kind 分）：
//   effect（.nce）→ 每 tick 叫 nce_render(frame_id, time_us)
//   task  （.nct）→ 每 tick 叫 task_tick()（節奏 = manifest fps）
//
// host imports（白名單，見 nc_mod_abi.h）：nc_print / nc_print_i32 /
// nc_gpio_read / nc_gpio_write / nc_gpio_toggle。guest 碰不到其他任何東西。
#include "nc_plugin.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "soc/soc_caps.h"
#include "wasm_export.h"

#include "nc_mod_abi.h"
#include "nc_ncd.h"     // NC_NCD_KIND_TASK

static const char *TAG = "plugin.wamr";

// 每次呼叫的 wasm operand stack。模組自己 recursion 太深會先撞到這個。
#define WAMR_CALL_STACK   8192
// 大過呢個數就優先由 PSRAM 要（linear memory 最少 64 KB）。
#define WAMR_SMALL_ALLOC  (24 * 1024)

// ───────────────────────── 實例（slot）─────────────────────────
//
// 一個插件一個 slot（module／instance／exec env／tick function）。
// 把手指向 slot 本身：客戶端只負責原封不動傳返入來，唔需要知裡面係咩。
// name 係 loader 抄過來的（src 的指標生命週期唔一定長過插件）。
typedef struct {
    wasm_module_t        module;
    wasm_module_inst_t   inst;
    wasm_exec_env_t      env;
    wasm_function_inst_t fn_tick;
    char                 name[NC_PLUGIN_NAME_MAX];
    uint32_t             fb_bytes;      // effect：framebuffer 大小（0 = 冇）
    bool                 is_task;
    bool                 used;
} wamr_slot_t;

static wamr_slot_t s_slots[NC_PLUGIN_MAX];
static bool s_rt_up;
static const char *s_last_err = "-";

// 正在執行的插件：只給 log 前綴用（WAMR 的 native API 冇「模組自訂 user data」）。
static wamr_slot_t *s_current;

static const char *cur_name(void)
{
    return (s_current != NULL) ? s_current->name : "?";
}

static wamr_slot_t *slot_alloc(void)
{
    for (int i = 0; i < NC_PLUGIN_MAX; i++) {
        if (!s_slots[i].used) {
            return &s_slots[i];
        }
    }
    return NULL;
}

static wamr_slot_t *slot_of(nc_plugin_t p)
{
    wamr_slot_t *slot = (wamr_slot_t *)p;
    if (slot < &s_slots[0] || slot >= &s_slots[NC_PLUGIN_MAX] || !slot->used) {
        return NULL;
    }
    return slot;
}

// ───────────────────────── 記憶體：WAMR → IDF heap ─────────────────────────
// WAMR 用 Alloc_With_Allocator，所有 allocation（含 linear memory）都經這裡。
// 大塊優先 PSRAM（S3 內部 RAM 好緊），細塊優先 internal（快、而且唔會把
// PSRAM 切碎）。兩邊都唔得就試另一邊，最後才交 NULL（WAMR 會自己報錯）。
static void *wamr_alloc(unsigned int size)
{
    const uint32_t caps_ps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    const uint32_t caps_in = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    void *p = NULL;

    if (size >= WAMR_SMALL_ALLOC) {
        p = heap_caps_aligned_alloc(16, size, caps_ps);
        if (p == NULL) {
            p = heap_caps_aligned_alloc(16, size, caps_in);
        }
    } else {
        p = heap_caps_aligned_alloc(16, size, caps_in);
        if (p == NULL) {
            p = heap_caps_aligned_alloc(16, size, caps_ps);
        }
    }
    return p;
}

static void *wamr_realloc(void *ptr, unsigned int size)
{
    if (ptr == NULL) {
        return wamr_alloc(size);
    }
    if (size == 0) {
        heap_caps_free(ptr);
        return NULL;
    }
    // 唔知原本幾大就唔可以靠 realloc：用 heap_caps 的 allocated size 自己搬。
    const size_t old = heap_caps_get_allocated_size(ptr);
    void *np = wamr_alloc(size);
    if (np == NULL) {
        return NULL;
    }
    memcpy(np, ptr, (old < size) ? old : size);
    heap_caps_free(ptr);
    return np;
}

static void wamr_free(void *ptr)
{
    heap_caps_free(ptr);
}

// ───────────────────────── host imports（白名單）─────────────────────────

// 只准安全腳位。呢條白名單係「能力邊界」的一部分，唔係建議：
//   0／3／45／46 = boot／JTAG strap   7／8 = 本板 UART
//   19／20 = USB OTG                 26–37 = SPI flash／octal PSRAM
//   43／44 = UART0 預設
static bool pin_allowed(int pin)
{
    if (pin < 0 || pin >= SOC_GPIO_PIN_COUNT) {
        return false;
    }
    switch (pin) {
    case 0:
    case 3:
    case 7:
    case 8:
    case 19:
    case 20:
    case 43:
    case 44:
    case 45:
    case 46:
        return false;
    default:
        break;
    }
    return !(pin >= 26 && pin <= 37);
}

// -1 = 未設定，0 = 輸入，1 = 輸出。第一次用某隻腳才 gpio_config()。
static int8_t s_pin_mode[SOC_GPIO_PIN_COUNT];

static int32_t host_print(wasm_exec_env_t env, int32_t ptr, int32_t len)
{
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(env);
    if (inst == NULL) {
        return -1;
    }
    if (len <= 0) {
        return 0;
    }
    if (len > 240) {
        len = 240;                     // 一行 log 唔好爆 ring
    }
    if (!wasm_runtime_validate_app_addr(inst, (uint32_t)ptr, (uint32_t)len)) {
        ESP_LOGW(TAG, "[%s] nc_print 越界（ptr=%d len=%d）", cur_name(), (int)ptr, (int)len);
        return -1;
    }
    const char *text = (const char *)wasm_runtime_addr_app_to_native(inst, (uint32_t)ptr);
    ESP_LOGI(TAG, "[%s] %.*s", cur_name(), (int)len, text);
    return 0;
}

static int32_t host_print_i32(wasm_exec_env_t env, int32_t v)
{
    (void)env;
    ESP_LOGI(TAG, "[%s] %d", cur_name(), (int)v);
    return 0;
}

// 一行過：<文字1><v1> <文字2><v2>。省掉「每次 print 都係獨立 log 行」的噪音。
static int32_t host_print2(wasm_exec_env_t env, int32_t p1, int32_t l1, int32_t v1,
                           int32_t p2, int32_t l2, int32_t v2)
{
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(env);
    if (inst == NULL) {
        return -1;
    }
    if (l1 < 0) {
        l1 = 0;
    }
    if (l2 < 0) {
        l2 = 0;
    }
    if (l1 > 120) {
        l1 = 120;
    }
    if (l2 > 120) {
        l2 = 120;
    }
    if (!wasm_runtime_validate_app_addr(inst, (uint32_t)p1, (uint32_t)l1) ||
        !wasm_runtime_validate_app_addr(inst, (uint32_t)p2, (uint32_t)l2)) {
        ESP_LOGW(TAG, "[%s] nc_print2 越界", cur_name());
        return -1;
    }
    const char *s1 = (const char *)wasm_runtime_addr_app_to_native(inst, (uint32_t)p1);
    const char *s2 = (const char *)wasm_runtime_addr_app_to_native(inst, (uint32_t)p2);
    ESP_LOGI(TAG, "[%s] %.*s%d %.*s%d", cur_name(), (int)l1, s1, (int)v1,
             (int)l2, s2, (int)v2);
    return 0;
}

// 時間：等於 MPY 的 time.ticks_us()。i64 返回（WAMR signature "()I"）。
static uint64_t host_now_us(wasm_exec_env_t env)
{
    (void)env;
    return (uint64_t)esp_timer_get_time();
}

static int32_t host_gpio_read(wasm_exec_env_t env, int32_t pin)
{
    (void)env;
    if (!pin_allowed(pin)) {
        ESP_LOGW(TAG, "[%s] gpio_read 拒絕腳位 %d", cur_name(), (int)pin);
        return -1;
    }
    if (s_pin_mode[pin] < 0) {
        const gpio_config_t cfg = {
            .pin_bit_mask = (1ULL << pin),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        if (gpio_config(&cfg) != ESP_OK) {
            return -1;
        }
        s_pin_mode[pin] = 0;
    }
    return gpio_get_level((gpio_num_t)pin);
}

static int32_t host_gpio_write(wasm_exec_env_t env, int32_t pin, int32_t value)
{
    (void)env;
    if (!pin_allowed(pin)) {
        ESP_LOGW(TAG, "[%s] gpio_write 拒絕腳位 %d", cur_name(), (int)pin);
        return -1;
    }
    if (s_pin_mode[pin] != 1) {
        const gpio_config_t cfg = {
            .pin_bit_mask = (1ULL << pin),
            // INPUT_OUTPUT：輸出同時保留輸入感測，否則 gpio_get_level() 永遠讀 0，
            // 模組「反轉 GPIO」會變成每次都係同一個方向（2026-11-17 實測）。
            .mode = GPIO_MODE_INPUT_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        if (gpio_config(&cfg) != ESP_OK) {
            return -1;
        }
        s_pin_mode[pin] = 1;
    }
    gpio_set_level((gpio_num_t)pin, value ? 1 : 0);
    return 0;
}

// 反轉：回新電平（0／1），失敗 -1。第一次用就設成輸出（保留目前電平）。
static int32_t host_gpio_toggle(wasm_exec_env_t env, int32_t pin)
{
    (void)env;
    if (!pin_allowed(pin)) {
        ESP_LOGW(TAG, "[%s] gpio_toggle 拒絕腳位 %d", cur_name(), (int)pin);
        return -1;
    }
    if (s_pin_mode[pin] != 1) {
        const int32_t before = gpio_get_level((gpio_num_t)pin);
        const gpio_config_t cfg = {
            .pin_bit_mask = (1ULL << pin),
            // INPUT_OUTPUT：輸出同時保留輸入感測，否則 gpio_get_level() 永遠讀 0，
            // 模組「反轉 GPIO」會變成每次都係同一個方向（2026-11-17 實測）。
            .mode = GPIO_MODE_INPUT_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        if (gpio_config(&cfg) != ESP_OK) {
            return -1;
        }
        s_pin_mode[pin] = 1;
        gpio_set_level((gpio_num_t)pin, before ? 0 : 1);
        return before ? 0 : 1;
    }
    const int32_t now = gpio_get_level((gpio_num_t)pin);
    gpio_set_level((gpio_num_t)pin, now ? 0 : 1);
    return now ? 0 : 1;
}

// **唔可以係 const**：WAMR 的 wasm_native_register_natives() 會 qsort() 呢個
// 陣列（wasm_native.c: register_natives → qsort(native_symbols, ...)）。
// 放喺 flash rodata 會被寫 → S3 報 "Cache error: Dbus write to cache rejected"
// → 開機 panic（2026-11-17 實測，backtrace 指到 register_natives）。
static NativeSymbol s_native[] = {
    { NC_MOD_IMPORT_PRINT,       (void *)host_print,       "(ii)i", NULL },
    { NC_MOD_IMPORT_PRINT_I32,   (void *)host_print_i32,   "(i)i",  NULL },
    { NC_MOD_IMPORT_PRINT2,      (void *)host_print2,      "(iiiiii)i", NULL },
    { NC_MOD_IMPORT_GPIO_READ,   (void *)host_gpio_read,   "(i)i",  NULL },
    { NC_MOD_IMPORT_GPIO_WRITE,  (void *)host_gpio_write,  "(ii)i", NULL },
    { NC_MOD_IMPORT_GPIO_TOGGLE, (void *)host_gpio_toggle, "(i)i",  NULL },
    { NC_MOD_IMPORT_NOW_US,      (void *)host_now_us,      "()I",   NULL },
};

// ───────────────────────── backend hooks ─────────────────────────

static esp_err_t call0_or_2(wamr_slot_t *slot, wasm_function_inst_t fn,
                            uint32_t argc, uint32_t *argv, const char *what)
{
    s_current = slot;
    const bool ok = wasm_runtime_call_wasm(slot->env, fn, argc, argv);
    s_current = NULL;
    if (!ok) {
        const char *ex = wasm_runtime_get_exception(slot->inst);
        ESP_LOGE(TAG, "[%s] %s trap：%s", slot->name, what, ex ? ex : "(unknown)");
        wasm_runtime_clear_exception(slot->inst);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t engine_init(void *arena, size_t bytes)
{
    (void)arena;
    (void)bytes;
    if (s_rt_up) {
        return ESP_OK;                 // soft reboot 之後再呼叫都唔會重入
    }
    for (int i = 0; i < SOC_GPIO_PIN_COUNT; i++) {
        s_pin_mode[i] = -1;
    }

    RuntimeInitArgs args;
    memset(&args, 0, sizeof(args));
    args.mem_alloc_type = Alloc_With_Allocator;
    args.mem_alloc_option.allocator.malloc_func = (void *)wamr_alloc;
    args.mem_alloc_option.allocator.realloc_func = (void *)wamr_realloc;
    args.mem_alloc_option.allocator.free_func = (void *)wamr_free;
    args.native_module_name = "env";
    args.native_symbols = (NativeSymbol *)s_native;
    args.n_native_symbols = sizeof(s_native) / sizeof(s_native[0]);
    args.running_mode = Mode_Interp;

    if (!wasm_runtime_full_init(&args)) {
        ESP_LOGE(TAG, "wasm_runtime_full_init 失敗（heap 唔夠？）");
        return ESP_ERR_NO_MEM;
    }
    s_rt_up = true;
    ESP_LOGI(TAG, "WAMR 起來：interp、imports=%u（nc_print/nc_print_i32/gpio_read/write/toggle）",
             (unsigned)args.n_native_symbols);
    return ESP_OK;
}

static esp_err_t engine_open(nc_plugin_t *out, const nc_plugin_src_t *src)
{
    if (out == NULL || src == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (!s_rt_up) {
        return ESP_ERR_INVALID_STATE;
    }
    if (src->fmt != NC_PLUGIN_FMT_WASM) {
        ESP_LOGW(TAG, "%s：AOT payload 未接（P3）→ 只收 wasm", src->name);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (src->payload == NULL || src->size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    wamr_slot_t *slot = slot_alloc();
    if (slot == NULL) {
        ESP_LOGW(TAG, "實例表滿了（%d）", NC_PLUGIN_MAX);
        return ESP_ERR_NO_MEM;
    }

    char err[128] = {0};
    wasm_module_t mod = wasm_runtime_load((uint8_t *)src->payload, (uint32_t)src->size,
                                          err, sizeof(err));
    if (mod == NULL) {
        s_last_err = "wasm_load";
        ESP_LOGE(TAG, "%s：wasm_runtime_load 失敗：%s", src->name, err);
        return ESP_ERR_INVALID_STATE;
    }
    wasm_module_inst_t inst = wasm_runtime_instantiate(mod, WAMR_CALL_STACK, 0,
                                                       err, sizeof(err));
    if (inst == NULL) {
        s_last_err = "wasm_instantiate";
        ESP_LOGE(TAG, "%s：instantiate 失敗：%s", src->name, err);
        wasm_runtime_unload(mod);
        return ESP_ERR_NO_MEM;
    }
    wasm_exec_env_t env = wasm_runtime_create_exec_env(inst, WAMR_CALL_STACK);
    if (env == NULL) {
        s_last_err = "exec_env";
        ESP_LOGE(TAG, "%s：create_exec_env 失敗", src->name);
        wasm_runtime_deinstantiate(inst);
        wasm_runtime_unload(mod);
        return ESP_ERR_NO_MEM;
    }

    const bool is_task = (src->kind == NC_NCD_KIND_TASK);

    // ABI v0：descriptor 寫在 guest 線性記憶體 offset 0（host 寫、guest 讀）。
    // effect 模組靠它才知道 px／framebuffer 在哪；task 模組唔用。
    if (!is_task) {
        nc_mod_desc_t desc;
        if (!nc_plugin_desc_build(src->px, src->mem_bytes, &desc)) {
            ESP_LOGE(TAG, "%s：px=%u 的 fb 放唔落 %u B 線性記憶體 → 拒絕",
                     src->name, (unsigned)src->px, (unsigned)src->mem_bytes);
            wasm_runtime_destroy_exec_env(env);
            wasm_runtime_deinstantiate(inst);
            wasm_runtime_unload(mod);
            return ESP_ERR_INVALID_SIZE;
        }
        uint8_t *base = wasm_runtime_addr_app_to_native(inst, 0);
        if (base == NULL) {
            ESP_LOGE(TAG, "%s：拿不到 linear memory → 拒絕", src->name);
            wasm_runtime_destroy_exec_env(env);
            wasm_runtime_deinstantiate(inst);
            wasm_runtime_unload(mod);
            return ESP_ERR_INVALID_STATE;
        }
        memcpy(base, &desc, sizeof(desc));       // 0..28（28..32 係 padding）
        slot->fb_bytes = desc.fb_bytes;
    }

    const char *tick_name = is_task ? NC_MOD_EXPORT_TASK_TICK : NC_MOD_EXPORT_RENDER;
    wasm_function_inst_t fn_tick = wasm_runtime_lookup_function(inst, tick_name);
    if (fn_tick == NULL) {
        s_last_err = "export";
        ESP_LOGE(TAG, "%s：缺 export %s（%s）", src->name, tick_name,
                 is_task ? "任務模組" : "效果模組");
        wasm_runtime_destroy_exec_env(env);
        wasm_runtime_deinstantiate(inst);
        wasm_runtime_unload(mod);
        return ESP_ERR_NOT_FOUND;
    }

    memset(slot, 0, sizeof(*slot));
    slot->module = mod;
    slot->inst = inst;
    slot->env = env;
    slot->fn_tick = fn_tick;
    slot->is_task = is_task;
    slot->used = true;
    // name 抄一份：src 的指標（loader 的 nc_mod_t）唔保證活過插件。
    size_t n = 0;
    if (src->name != NULL) {
        for (; n + 1 < sizeof(slot->name) && src->name[n] != '\0'; n++) {
            slot->name[n] = src->name[n];
        }
    }
    slot->name[n] = '\0';

    // 可選 init（一次）。冇就唔叫。
    const char *init_name = is_task ? NC_MOD_EXPORT_TASK_INIT : NC_MOD_EXPORT_INIT;
    wasm_function_inst_t fn_init = wasm_runtime_lookup_function(inst, init_name);
    if (fn_init != NULL) {
        uint32_t argv[2] = {0, 0};
        // effect 的 nce_init(desc_ptr)：desc 在 offset 0，所以傳 0。
        const uint32_t argc = is_task ? 0 : 1;
        if (call0_or_2(slot, fn_init, argc, argv, init_name) != ESP_OK) {
            ESP_LOGW(TAG, "%s：%s 出錯（照樣載入，tick 仍然會叫）",
                     slot->name, init_name);
        }
    }

    ESP_LOGI(TAG, "插件 %s 已實例化（%s，payload %u B，tick=%s）",
             slot->name, is_task ? "task" : "effect", (unsigned)src->size, tick_name);
    *out = slot;
    return ESP_OK;
}

static esp_err_t engine_step(nc_plugin_t p, uint32_t frame_id)
{
    // OTA 進行中（BEGIN 抹分區 → END 驗證完）就跳過：WAMR 喺 flash 執行程式碼，
    // 同時間 flash erase/write 會 park 另一核、暫停 cache → 實測 task watchdog
    // 5 s 到期 reboot（2026-11-17）。模組唔會 miss 任何嘢：host 呢段時間只等 ACK。
    if (nc_plugin_paused()) {
        return ESP_OK;
    }

    wamr_slot_t *slot = slot_of(p);
    if (slot == NULL || slot->fn_tick == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t argv[2] = {0, 0};
    uint32_t argc = 0;
    if (!slot->is_task) {
        // 效果：nce_render(frame_id, time_us)
        argc = 2;
        argv[0] = frame_id;
        argv[1] = (uint32_t)esp_timer_get_time();
    }

    const char *what = slot->is_task ? NC_MOD_EXPORT_TASK_TICK : NC_MOD_EXPORT_RENDER;
    const esp_err_t err = call0_or_2(slot, slot->fn_tick, argc, argv, what);
    if (err != ESP_OK) {
        return err;
    }
    // 回傳值：0 = ok；非 0 = 模組自報錯誤（契約見 ncm-contract §5）。
    if (argv[0] != 0) {
        ESP_LOGW(TAG, "[%s] %s 回 %d", slot->name, what, (int)argv[0]);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t engine_frame(nc_plugin_t p, const uint8_t **fb, uint32_t *bytes)
{
    wamr_slot_t *slot = slot_of(p);
    if (slot == NULL || slot->fb_bytes == 0 || slot->inst == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    uint8_t *base = wasm_runtime_addr_app_to_native(slot->inst, NC_PLUGIN_FB_OFF);
    if (base == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    *fb = base;
    *bytes = slot->fb_bytes;
    return ESP_OK;
}

static void engine_close(nc_plugin_t p)
{
    wamr_slot_t *slot = slot_of(p);
    if (slot == NULL) {
        return;
    }
    if (slot->env != NULL) {
        wasm_runtime_destroy_exec_env(slot->env);
    }
    if (slot->inst != NULL) {
        wasm_runtime_deinstantiate(slot->inst);
    }
    if (slot->module != NULL) {
        wasm_runtime_unload(slot->module);
    }
    ESP_LOGI(TAG, "插件 %s 的 WAMR 資源已回收", slot->name);
    memset(slot, 0, sizeof(*slot));
}

static void engine_deinit(void)
{
    for (int i = 0; i < NC_PLUGIN_MAX; i++) {
        if (s_slots[i].used) {
            engine_close(&s_slots[i]);
        }
    }
    if (s_rt_up) {
        wasm_runtime_destroy();
        s_rt_up = false;
    }
}

static const nc_plugin_backend_t s_wamr_backend = {
    .name = "wamr",
    .init = engine_init,
    .open = engine_open,
    .step = engine_step,
    .frame = engine_frame,
    .close = engine_close,
    .deinit = engine_deinit,
};

const char *nc_plugin_wamr_last_error(void)
{
    return s_last_err;
}

const nc_plugin_backend_t *nc_plugin_wamr_backend(void)
{
    return &s_wamr_backend;
}
