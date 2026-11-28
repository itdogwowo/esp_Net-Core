// nc_ncm_boot.c — ncm 的開機接線（**測試能力**；release 完全唔編）。
//
// 2026-11-28 由 main.c 搬出來（見 docs/00_pm/gap-review.md §6.4.1）：
//   · 插件工具（解碼＋運行）住 `sw/plugin`，完全唔識 main／task；
//   · 載入器政策住 `host/ncm`（掃描／arena／live／預算／attach）；
//   · 「接去開機框架」這一段（任務登記、arena 宣告、載入時機、status 區塊）
//     就住這裡 —— 全部用「元件自己現身」的方式，所以 main 完全唔知有 ncm：
//       NC_TASK_AUTO_REG      任務登記
//       nc_task_post_register start_all 之後才載入模組
//       nc_status_register    STATUS_GET 的 "ncm" 區塊
//
// 為什麼仍然要另開 8 KB task 載入：main task 棧只有 3,072 B，WAMR 的
// full_init／load／instantiate 會用幾 KB C 棧 → stack overflow → reboot loop。
#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "nc_mod.h"
#include "nc_plan.h"
#include "nc_ota_action.h"
#include "nc_plugin.h"
#include "nc_px_nce.h"
#include "nc_res.h"
#include "nc_status.h"
#include "nc_task.h"

static const char *TAG = "ncm";

#define NCM_TASK_CORE 1
// 測試任務的優先級要**低過** UART producer（prio 3）：同級會時間切片（10 ms/tick），
// ncm 每 10 ms 醒一次，剛好把 uart.rx 壓住一個 tick —— 1 KB UART RX 環（11 ms）
// 會因此偶發溢位（實測 15 分鐘 soak 4 次 CRC）。降到 2 之後 uart.rx 永遠先跑。
#define NCM_TASK_PRIO 2

// 掃描結果：declare 時掃一次，載入時用同一份（B06 → B08 之間唔會變）。
static nc_mod_scan_t s_scan;
static SemaphoreHandle_t s_boot_done;

// 每 tick：效果插件的 framebuffer → pixel 值流信箱（`.nce` 合成）。
// （消費端 wiring 見 gap-review §6.7；呢一步只做「產出」。）
static void ncm_nce_publish(void *plugin)
{
    const uint8_t *fb = NULL;
    uint32_t bytes = 0;
    if (plugin == NULL || nc_plugin_frame((nc_plugin_t)plugin, &fb, &bytes) != ESP_OK) {
        return;
    }
    if (fb == NULL || bytes == 0) {
        return;
    }
    (void)nc_px_nce_init(bytes);           // 冪等：同一個大小唔會重新要記憶體
    (void)nc_px_nce_publish(fb, bytes);
}

static const nc_task_provide_t s_provides[] = {
    { .name = "ncm.info", .kind = NC_PROVIDE_JSON, .desc = "live 模組 / arena / 每個實例",
      .json = nc_mod_info_json },
};

// ── ① 宣告：掃描＋arena 大小（原本喺 main 的 B06）──
static int ncm_declare(nc_task_res_t *out, int max)
{
    if (max < 1) {
        return 0;
    }
    // 只掃一次：declare_all 每個任務只會叫一次 declare，但防禦性保留。
    if (s_scan.count == 0) {
        nc_mod_scan_all(&s_scan);
        nc_res_build();
        nc_mod_declare_res(&s_scan);
        nc_res_merge();
        nc_mod_merge_cfg(&s_scan);
    }

    // 掃到 N 個模組、檔案合計 B bytes → arena = clamp(B + N×1 KB, 32 KB, 512 KB)。
    const size_t arena = nc_mod_arena_bytes(&s_scan);
    if (arena == 0) {
        ESP_LOGI(TAG, "ncm arena：0 個模組 → 唔宣告（省 32 KB PSRAM；加 .nce 後重開機才要）");
        return 0;
    }
    ESP_LOGI(TAG, "ncm arena：掃到 %d 個模組、檔案 %u B → 宣告 %u B（上限 %u B）",
             s_scan.count, (unsigned)s_scan.bytes, (unsigned)arena,
             (unsigned)NC_MOD_ARENA_MAX);
    out[0] = (nc_task_res_t){
        .name = "ncm.arena", .bytes = arena,
        .cls = NC_PLAN_CLS_BULK, .align = 32,
        // 整塊 arena 是「一格」的緩衝區（`.nct` / `.nce` 之後用 nc_buf_carve 由它切）
        .slot_bytes = arena, .n = 1, .flags = 0,
    };
    return 1;
}

// ── ② 載入（start_all 之後；見 nc_task_post_run）──
static void ncm_boot(void)
{
    nc_plugin_set_pause_check(nc_ota_action_paused);   // OTA 期間唔跑插件
    const esp_err_t berr = nc_plugin_set_backend(nc_plugin_wamr_backend());
    if (berr == ESP_OK) {
        const int n = nc_mod_load_all(&s_scan);
        ESP_LOGI(TAG, "ncm 模組載入：%d 個（%d 個被拒）", n, s_scan.bad);
    } else {
        ESP_LOGW(TAG, "WAMR backend 起唔到（%s）→ 模組只登記、唔跑",
                 esp_err_to_name(berr));
        (void)nc_mod_load_all(&s_scan);
    }
}

static void ncm_boot_task(void *arg)
{
    (void)arg;
    ncm_boot();
    if (s_boot_done != NULL) {
        xSemaphoreGive(s_boot_done);
    }
    vTaskDelete(NULL);
}

static void ncm_post_start(void)
{
    if (!nc_task_started("ncm")) {
        return;                         // 冇 arena（冇模組）→ 任務唔存在
    }
    s_boot_done = xSemaphoreCreateBinary();
    if (s_boot_done != NULL &&
        xTaskCreate(ncm_boot_task, "ncm.boot", 8192, NULL, 3, NULL) == pdPASS) {
        xSemaphoreTake(s_boot_done, portMAX_DELAY);
    } else {
        ESP_LOGW(TAG, "ncm.boot task 開唔到 → 喺呼叫者 task 直接載入（棧風險）");
        ncm_boot();
    }
    vSemaphoreDelete(s_boot_done);
    s_boot_done = NULL;
}

// ── ③ 啟動：arena 交插件工具，登記 post-start 同 status ──
static esp_err_t ncm_on_start(const nc_task_res_view_t *res, int n)
{
    if (n < 1 || res == NULL) {
        ESP_LOGI(TAG, "ncm：冇模組 → 唔啟動（0 B arena；加 .nce／.nct 後重開機）");
        return ESP_ERR_NOT_FOUND;
    }
    if (res[0].ptr == NULL) {
        return ESP_ERR_NO_MEM;
    }
    nc_mod_set_task_placement(NCM_TASK_CORE, NCM_TASK_PRIO);
    const esp_err_t e = nc_mod_init(res[0].ptr, res[0].len);
    if (e != ESP_OK) {
        return e;
    }
    // 兩個註冊都要喺 start_all 之內完成：main 之後才 nc_task_post_run()、
    // 主機之後才問 STATUS_GET。
    nc_mod_set_tick_hook(ncm_nce_publish);
    (void)nc_task_post_register(ncm_post_start);
    (void)nc_status_register("ncm", nc_mod_status_json);
    return ESP_OK;
}

static const nc_task_loop_t s_loops[] = {
    { .name = "ncm", .fn = nc_mod_task, .arg = NULL,
      // 8 KB：tick 會經 wasm_runtime_call_wasm 入 WAMR 直譯器（C 棧），
      // 4 KB 對 task_tick + native（print/gpio）太貼。exec env 自己的 operand
      // stack 另計（WAMR_CALL_STACK）。
      .stack = 8192, .prio = NCM_TASK_PRIO, .core = NCM_TASK_CORE,
      // period_ms = 0：幀週期由 manifest 的 fps 決定，nc_mod_task 自己延遲。
      .layer = 3, .period_ms = 0 },
};

static const nc_task_t s_task = {
    // ncm 是載入器／renderer，屬核心基建（房東），不是租戶：唔寫入 tasks.json。
    .name = "ncm", .kind = NC_TASK_SYSTEM, .level = 4, .order = 30,
    .declare = ncm_declare, .on_start = ncm_on_start,
    .loops = s_loops, .nloops = sizeof(s_loops) / sizeof(s_loops[0]),
    .provides = s_provides, .nprovides = sizeof(s_provides) / sizeof(s_provides[0]),
};
NC_TASK_AUTO_REG(ncm, &s_task);
