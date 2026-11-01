# 怎麼寫一個 Service（C 版）

> **適用**：`nc_service`（服務管理器）＋ `nc_plan`（資源管理器）＋ `nc_task`（任務表）
> **相關**：[rx-architecture.md](rx-architecture.md) §11（為什麼這樣分）、[memory.md](memory.md)
> **狀態**：服務管理器已實作並上板（`0x1101 keys=svc` 可查）；`stop` / 資源歸還還是半套（見 §6）

---

> **先分清楚**：NC4 通道、硬體、driver 屬於 **System**（固定順序，見 [config-layers.md](config-layers.md)），
> 不是 Service。這份文件講的是「應用服務」（效果、UI、慢工作、自測）。

## 1. 生命週期：三個動詞 ＋ 一個迴圈

```
   開機                          carve 之後                   跑                     停機
    │                              │                          │                      │
declare() ──► nc_plan_commit ──► start() ──► nc_tasks_boot() ──► task loop ──► stop()
 寫需求                            交指標                     依 layer 啟動          收尾
 登記 task                        建 hub / 接狀態
```

| 階段 | 你寫的函式 | 什麼時候被呼叫 | 你可以做什麼 |
|---|---|---|---|
| **① declare** | `int (*declare)(nc_res_req_t*, int max)` | 開機，`nc_plan_commit()` **之前** | 填資源需求、`nc_task_register()` 你的 loop |
| **② start** | `esp_err_t (*start)(const nc_res_view_t*, int n)` | commit 之後、task 啟動之前 | 用交進來的指標建 hub / 開 socket / 初始化狀態 |
| **③ run** | 你的 task 函式 | `nc_tasks_boot()` 依 layer 啟動 | 一般 FreeRTOS 迴圈 |
| **④ stop** | `void (*stop)(void)`（可選） | `nc_service_stop_all()`（反向） | 收尾；此時不要再碰資源 |

**只有這三個 callback**。沒有 init/loop/exit 的物件魔法，也沒有 C++ 建構子。

---

## 2. 跟「平常寫 FreeRTOS」不一樣的地方

| 平常 | 這裡 | 為什麼 |
|---|---|---|
| `malloc()` 要緩衝 | 在 `declare()` 寫 `nc_res_req_t` | 開機一次 carve、逐項回報、不會碎片化 |
| `xTaskCreate()` | `nc_task_register()` | 管理層要知道你跑在哪一核、哪一層，才能排啟動順序 |
| 自己 `nc_plan_get()` | `start()` 把指標交給你 | 取用只有一個地方（管理器），才知道誰拿到、誰失敗 |
| 直接呼叫別的服務 | 透過共用的 hub / handle / 事件 | 服務之間不互相 include |
| `vTaskDelay` 當等待 | 等資料用 blocking I/O / `select`；等交換用 `taskYIELD` | 1 tick 在小幀上是 8 倍的代價（踩過） |
| 全域變數滿天飛 | 狀態放自己的 .c 的 static，查詢用 callback | 目前還沒強制，但請照這個寫 |

---

## 3. 完整範例：`pulse`（每 10 ms 產生一個數值）

```c
// components/app/pulse/pulse_svc.c
#include <stdint.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nc_hub.h"
#include "nc_service.h"
#include "nc_task.h"

static const char *TAG = "pulse";
static nc_hub_t *s_hub;
static volatile uint32_t s_ticks;

// ── ③ run：一般的 FreeRTOS 迴圈 ──
static void pulse_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t v = ++s_ticks;
        uint32_t *slot = nc_hub_write_begin(s_hub);   // 沒空位就跳過這一輪（不等待）
        if (slot != NULL) {
            *slot = v;
            nc_hub_write_commit(s_hub);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ── ① declare：資源 + 任務（都在 commit 之前）──
static int pulse_declare(nc_res_req_t *out, int max)
{
    if (max < 1) {
        return 0;
    }
    out[0] = (nc_res_req_t){
        .name = "pulse.hub", .bytes = 16 * sizeof(uint32_t),
        .cls = NC_PLAN_INTERNAL, .align = 32,
    };
    nc_task_register(&(nc_task_def_t){
        .name = "pulse", .fn = pulse_task, .arg = NULL,
        .stack = 3072, .prio = 3, .core = 0, .layer = 2, .hw = NC_TASK_HW_NONE,
    });
    return 1;                       // 我宣告了 1 項資源
}

// ── ② start：資源由管理器交進來，不要自己 get ──
static esp_err_t pulse_start(const nc_res_view_t *res, int n)
{
    (void)n;
    s_hub = nc_hub_create_in(res[0].ptr, res[0].len, "pulse",
                             sizeof(uint32_t), 16, 32);
    if (s_hub == NULL) {
        return ESP_FAIL;            // 失敗 → 這個服務不啟動，其他照跑
    }
    ESP_LOGI(TAG, "pulse 啟動（hub %u B）", (unsigned)res[0].len);
    return ESP_OK;
}

// ── ④ stop：可選 ──
static void pulse_stop(void)
{
    s_hub = NULL;                   // task 已停，資源由管理器處理
}

const nc_service_t pulse_service = {
    .name = "pulse",
    .level = 2,                     // 0 = 系統必需，數字小的先啟動
    .declare = pulse_declare,
    .start = pulse_start,
    .stop = pulse_stop,
};
```

在 `main.c` 的裝配區塊加一行：

```c
nc_service_add(&pulse_service);
```

然後既有的順序就會自動處理：

```c
nc_plan_reset();
nc_service_add(...);            // 你的一行
nc_service_declare_all();       // 呼叫每個 declare() → plan_declare
nc_plan_commit(avail, false);   // 每 class 一次 carve
nc_service_start_all();         // 取得資源 → start()
nc_tasks_boot();                // 依 layer 啟動 task
```

查詢（序列埠或 TCP 都行）：

```
0x1101 keys=svc
→ {"svc":[{"name":"pulse","level":2,"started":1,
           "res":[{"name":"pulse.hub","bytes":64,"ok":1}]}, ...]}
```

---

## 4. 資源怎麼宣告

```c
nc_res_req_t r = {
    .name  = "pulse.hub",          // 全域唯一；查詢、報表都用它
    .bytes = 16 * sizeof(uint32_t),// 你要多少
    .cls   = NC_PLAN_INTERNAL,     // DMA / INTERNAL / PSRAM
    .align = 32,                   // 0 = 32
};
```

| class | 什麼時候用 |
|---|---|
| `NC_PLAN_DMA` | 要給 DMA 的緩衝（EMAC、SPI、I2S） |
| `NC_PLAN_INTERNAL` | 一般內部 RAM（最快；當交換緩衝用這個） |
| `NC_PLAN_PSRAM` | 大塊、不常存取（assets、影像） |

`declare()` 回傳你寫了幾項（≤ `max`）。寫超過 `NC_SERVICE_MAX_RES`（8）會被截斷。

---

## 5. 檢查清單（送 code review 前）

- [ ] `declare()` 裡**沒有** `malloc`、**沒有** `xTaskCreate`
- [ ] `start()` 裡**沒有** `nc_plan_get()`（用參數給的 `res[]`）
- [ ] 每個 task 都有 `nc_task_register()`，且 `layer` 比它所依賴的服務大
- [ ] `start()` 失敗會回 `ESP_FAIL`，而且**不會**留下半啟動的狀態
- [ ] loop 裡沒有 `while(1){ if(沒資料) vTaskDelay(1); }` 這種輪詢
- [ ] 有 hub/queue 的地方：`write_begin` 拿不到就跳過，不等待
- [ ] 資源名稱是全系統唯一的（用 `<服務>.<用途>`）

---

## 6. 目前限制（誠實寫在這裡）

| 限制 | 影響 | 什麼時候補 |
|---|---|---|
| `nc_plan` 沒有 release | 停機不會還記憶體（重啟服務會拿不到） | 等「暫停/恢復服務」的需求出現 |
| `nc_task` 沒有 stop | task 只能跑到斷電；`stop()` 目前只是介面預留 | 下一步（`nc_task_stop` 需要合作式停止） |
| 服務之間沒有依賴宣告 | 只靠 `level` 排序 | 有真正的依賴圖再說 |
| 動態服務（執行期新增） | 目前只支援開機登記 | 見 rx-architecture §11.3 的例外路徑 |

---

## 7. 最小範本（複製這份開始寫）

```c
#include "nc_service.h"
#include "nc_task.h"

static int  my_declare(nc_res_req_t *out, int max);
static esp_err_t my_start(const nc_res_view_t *res, int n);
static void my_stop(void);
static void my_task(void *arg);

const nc_service_t my_service = {
    .name = "my", .level = 2,
    .declare = my_declare, .start = my_start, .stop = my_stop,
};

static int my_declare(nc_res_req_t *out, int max)
{
    if (max < 1) return 0;
    out[0] = (nc_res_req_t){ .name = "my.buf", .bytes = 1024,
                             .cls = NC_PLAN_INTERNAL, .align = 32 };
    nc_task_register(&(nc_task_def_t){
        .name = "my", .fn = my_task, .arg = NULL, .stack = 3072,
        .prio = 3, .core = 0, .layer = 2, .hw = NC_TASK_HW_NONE,
    });
    return 1;
}

static esp_err_t my_start(const nc_res_view_t *res, int n)
{
    (void)n; (void)res;
    return ESP_OK;
}

static void my_stop(void) {}

static void my_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
```
