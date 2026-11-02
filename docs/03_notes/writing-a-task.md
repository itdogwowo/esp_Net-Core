# 怎麼寫一個 Task（任務管理器）

> **適用**：`nc_task`（任務宣告）＋ `nc_task_*`（任務管理器）
> **相關**：[fx-loader.md](fx-loader.md)（檔案載入的任務）、[extension-architecture.md](extension-architecture.md)、[hardware-bus.md](hardware-bus.md)
> **狀態**：已實作（2026-11-02）。host 測試：`tools/hosttest/task_test/`。
> **舊名**：這份原本叫「怎麼寫一個 Service」，還有一段時間把 service / task 分成兩層 ——
> 那是命名造成的誤會，**兩者是同一件事**。現在只有 `nc_task_t`（`kind = NC_TASK_NORMAL`）。

---

## 0. 一句話

**你只寫一份宣告 + 一個 loop，呼叫 `nc_task_register()`，剩下的管理器做。**

管理器（`nc_task_*`）負責：

| 管理器做的事 | 你原本要自己做的 |
|---|---|
| 依 `level` 排開機順序 | 在 main 裡排呼叫順序 |
| 把 `mem[]` / `declare()` 送進 `nc_plan`、取回指標 | `nc_plan_declare` / `nc_plan_get` |
| 開 / 停 FreeRTOS task、綁核（core 由你決定，不改寫） | `xTaskCreatePinnedToCore` |
| 量每輪時間（count / last / avg / max / 超支） | 自己加計時器 |
| 開機帳本、狀態 JSON、runtime 換核 / 暫停 | 各寫一份 |

---

## 1. 一個 Task 長什麼樣

```c
static esp_err_t my_on_start(const nc_task_res_view_t *res, int nres);   // 資源到手
static void      my_loop(void *arg);                                // 迴圈本體
static void      my_on_stop(void);                                  // 收尾

static const nc_task_loop_t my_loops[] = {
    { .name = "my", .fn = my_loop, .stack = 3072, .prio = 3,
      .core = 0, .layer = 2, .period_ms = 10 },
};

static const nc_task_t my_task = {
    .name = "my", .kind = NC_TASK_NORMAL, .level = 2,
    .declare = my_declare,          // 資源（可留空）
    .on_start = my_on_start,
    .on_stop  = my_on_stop,         // 可留空
    .loops = my_loops, .nloops = 1,
};

// main 的裝配區
nc_task_register(&my_task);
```

四個欄位群，剛好對應四個問題：

| 群 | 欄位 | 回答 |
|---|---|---|
| 身份 | `name` / `kind` / `level` | 我叫什麼、是哪一類、什麼時候起 |
| 資源 | `mem[]` / `declare()` / `claims[]` / `provides[]` | 我要什麼、我給什麼 |
| 執行 | `loops[]` | 跑哪一核、哪一層、多快、多少 stack |
| 生命 | `on_start` / `on_stop` | 起的時候做什麼、停的時候收什麼 |

### 1.1 `kind` 只有三種

| kind | 是什麼 | 能不能卸載 |
|---|---|---|
| `NC_TASK_SYSTEM` | 硬體 / driver / 通道 / 指令表（韌體的一部分） | ❌ 固定順序 |
| `NC_TASK_NORMAL` | 一般任務（**以前的 service 就是這個**） | ❌ 開機登記的不卸載 |
| `NC_TASK_EFFECT` | 由 `.fxe` 載入的任務 | ✅ runtime attach / detach |

### 1.2 為什麼是 `loops[]` 而不是一個 loop

九成的任務只有一個 loop。但 `nc4` 要兩個（`decode0`、`decode1` 各綁一核），
所以宣告是陣列；`loops[]` 是**執行單位**，`nc_task_t` 是**管理單位**（資源、生命週期共用）。

---

## 2. 生命週期：四步

```text
  ① register            ② declare              ③ start               ④ boot
  ──────────────────────────────────────────────────────────────────────────
  nc_task_register() → nc_task_declare_all() → nc_task_start_all() → nc_task_boot()
  只登記、排序          mem[] → nc_plan        資源到手 → on_start  依 layer 開 loop
                        + 帳本檢查              → 指令 → 準備 loop   （0 先）
```

**為什麼 `start` 和 `boot` 分開**：`on_start()` 必須在「所有任務的資源都配好」之後、
「任何 loop 開始跑」之前完成。例：`fx` 要先拿到 arena，render loop 才能開。

停止方向相反：

```text
nc_task_stop_all() / nc_task_detach()
  → 停 loop（等它自己看到 nc_task_exit_requested() 返回）
  → on_stop()
  → 收回指令（nc4_decoder_off_owner）
  → 收回 provides
```

**loop 一定要看結束旗標**，否則管理器停不下來（會回 `ESP_ERR_TIMEOUT` 且不強殺）：

```c
static void my_loop(void *arg)
{
    while (!nc_task_exit_requested()) {
        /* ... 做事 ... */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
```

---

## 3. 資源、指令、接線、提供

### 3.1 資源（`mem[]` / `declare()`）

```c
static int my_declare(nc_task_res_t *out, int max)
{
    if (max < 1) return 0;
    out[0] = (nc_task_res_t){ .name = "my.buf", .bytes = 1024,
                             .cls = NC_PLAN_INTERNAL, .align = 32 };
    return 1;
}
```

| class | 什麼時候用 |
|---|---|
| `NC_PLAN_DMA` | 要給 DMA 的緩衝 |
| `NC_PLAN_INTERNAL` | 一般內部 RAM（最快） |
| `NC_PLAN_PSRAM` | 大塊、不常存取 |

`on_start()` 的 `res[i]` 對應你宣告的第 i 塊（先 `mem[]` 再 `declare()`）。
**不要自己呼叫 `nc_plan_get()`** —— 取用只有一個地方，才知道誰拿到、誰失敗。

### 3.2 指令＝action，不是任務的財產

**任務不擁有指令。** 指令是系統的核心執行能力：decoder 持有一張 cmd → handler 的表，
開機時註冊一次，之後任何通道收到那一幀就分派給它。任務只負責「把某條通道/某個節拍
恆常地跑起來」，讓這些能力穩定可用。

```text
開機：  actions_register(decoder)      ← 一次，沒有人參與
          ├─ core：REBOOT / STATUS / bench / SET_MASTER …
          ├─ ota ：0x22xx
          └─ file：0x2005 / 0x1103

執行：  decode0 / decode1（任務）       ← 只是「收 → 解 → 分派」的迴圈
          └─ 收到 0x22xx → 呼叫 ota 的 handler（在解碼核上）
```

三層分工：

| 東西 | 是什麼 | 在哪裡 |
|---|---|---|
| **decoder + dispatch 表** | 位元組 → frame → 查表 → 呼叫 handler | `sys/nc4`（每條通道一個 parser，一張全域表） |
| **action** | 一條指令做的事：驗參數、選機制、回結果 | `components/action/*`（`ota_action` / `file_action` …）與 `main.c` |
| **task** | 把能力恆常化：收發迴圈、渲染迴圈、節拍 | `components/sys/tasks` 管理，宣告在 `main.c` |

**handler 跑在哪一核**：跟著**解碼任務**跑。`decode0`/`decode1` 各綁一核，
哪個核收到這一幀，handler 就在那核執行。而且 `nc4.c` 呼叫 handler 時拿
`s_dispatch_mutex` → **兩個解碼核的 handler 互斥**（一次只有一個在跑）。

**新增一條指令**：寫一個 action 模組（`components/action/<domain>/`），提供一個
`void <domain>_register(nc4_decoder_t *dec)`，在 `main.c` 的 `actions_register()`
裡呼叫一次。不需要建立任務，也不需要動 `nc_task_t`。

### 3.3 接線（`claims[]`）

```c
static const nc_task_claim_t my_claims[] = {
    { .id = "pin:16", .exclusive = true },
};
```

`exclusive` 的 id 在不同任務之間重複 → 帳本擋下後者。`false` = 共用（自己協調）。

> `claims[]` 只回答「誰宣告要用這個資源」。所有權本身是**裝配層人工分配**的
> （一個裝置 / 一條 bus 一個 owner），不是執行期搶的；見 §3.5。

### 3.4 提供（`provides[]`）—— 我給別人什麼

`claims[]` 是「我要什麼」；`provides[]` 是它的鏡像。沒有它，消費者只能 include 你的 `.c`
或猜你的 buffer 佈局。

```c
static bool prov_my_buf(void **ptr, size_t *bytes)
{
    if (s_buf == NULL) return false;      // 還沒 on_start → 還沒有
    *ptr = s_buf; *bytes = s_buf_len;
    return true;
}

static const nc_task_provide_t my_provides[] = {
    { .name = "my.buf",  .kind = NC_PROVIDE_BUF,  .desc = "我的緩衝", .buf = prov_my_buf },
    { .name = "my.info", .kind = NC_PROVIDE_JSON, .desc = "我的狀態", .json = my_info_json },
};
```

| kind | 取值方式 | 給什麼 |
|---|---|---|
| `NC_PROVIDE_BUF` | `bool (*)(void **ptr, size_t *bytes)` | 記憶體（framebuffer、ring、快照） |
| `NC_PROVIDE_JSON` | `size_t (*)(char *out, size_t cap)` | 即時查詢（狀態、統計） |
| `NC_PROVIDE_U32` | `bool (*)(uint32_t *out)` | 單一數值 |

**為什麼是 getter**：緩衝要等 `on_start()` 才有，之後還可能換位置。
**生命週期**：`on_start` 成功才登記，stop / detach 就消失 —— 不會拿到懸空 buffer。

消費端：

```c
void *buf; size_t n;
if (nc_provide_buf("my.buf", &buf, &n)) { /* 讀 n bytes */ }

char json[256];
if (nc_provide_json("fx.info", json, sizeof(json)) > 0) { /* ... */ }
```

> ⚠️ `BUF` 的生命週期綁在提供者身上：不要跨提供者的 stop / detach 使用。

### 3.5 硬體：找 owner，不要越過去

**現在沒有硬體管理器**（`nc_hw` 是設計紀錄，未實作，見 [hardware-bus.md](hardware-bus.md)）。
目前的規矩只有一條：

> **一個裝置 / 一條 bus 只有一個 owner；其他人要用，透過 owner 的介面。**

| 你要什麼 | 怎麼做 |
|---|---|
| 讀一個由別人擁有的裝置 | 找那個任務的 `provides[]`（buffer / JSON / u32） |
| 請它做一件事 | 送進它的 hub / 佇列（有 `nc_hub` 就用 hub） |
| 兩個模組共用一條 bus | **不要**；由 bus 的 owner 統一管 CS 與時序 |
| 你的任務自己擁有一個裝置 | 直接在自己的 loop 裡呼叫 driver（你就是 owner） |

`claims[]` 是宣告「我要用這個資源」，讓開機帳本擋下重複宣告 —— 它不負責序列化，
因為序列化在「一個 owner」這個設計裡就已經成立了。

> 哪天真的出現「同一裝置必須有兩個 owner」，才需要 task 層的硬體管理器；
> 觸發條件與形狀寫在 [hardware-bus.md](hardware-bus.md)。

### 3.6 緩衝：handler 不用自己管，但要知道邊界

**收進來的資料是「借你看」的，不是給你的**：

```c
static void on_xxx(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    // f->data 指向 parser 的緩衝（零複製），只在這個函式執行期間有效
    uint16_t v = f->data[0] | (f->data[1] << 8);
    // 要留到之後 → 自己複製走
}
```

**回覆用的緩衝是靜態或堆疊**，不進 plan：

| 地方 | 用什麼 | 大小 |
|---|---|---|
| `main.c` 的 `reply_json` | `static payload[2102]` + 堆疊 frame | ~4 KB 堆疊 |
| `file_action` | `static s_tx[NC4_MAX_FRAME]` | 8 KB BSS |
| `ota_action` | 堆疊 `frame[...]` | ~270 B |

**為什麼靜態 buffer 安全**：`nc4.c` 呼叫 handler 時持有 `s_dispatch_mutex`，
所以兩個解碼核的 handler **不會同時執行** —— 靜態 TX buffer 不會被另一個核蓋掉。

**什麼才要進 plan**：長命的東西 —— parser 區（`nc4.rx` / `uart.rx` / `tcp.rx`）、
hub、arena、DMA 槽。一次性的 frame / 回覆不需要，也不該進。

注意兩個代價：
- `reply_json` 需要約 4 KB 堆疊 → `decode0/1` 的 stack 是 6144，不是 4096。
- `file_action` 的 8 KB 是 BSS，不是 plan 管的 —— 所以 `keys=mem` 看不到它。

例外：`0xF001` 那組 bench handler 會 `malloc/free`（那是量測工具，不是常規路徑）。

---

## 4. 管理器幫你量了什麼

每個 loop 都會記：

| 欄位 | 意思 |
|---|---|
| `count` | 跑了幾輪 |
| `last_us` | 上一輪花多久 |
| `avg_us` | 平均 |
| `max_us` | 最久的一輪 |
| `over` | 超過 `budget_us` 的次數 |
| `wrap_us` | **管理器自己的成本**（整輪 − fn，含兩次 `esp_timer_get_time()`） |
| `wrap_avg_us` | 上面那個的平均 |

`STATUS_GET keys=tasks`：

```json
[{"name":"fx","kind":"normal","level":4,"started":1,"runtime":0,
  "mem":1,"claims":0,"provides":1,
  "loops":[{"name":"fx","core":1,"layer":3,"prio":3,"period_ms":0,
            "running":1,"enabled":1,"count":1606,"last_us":4100,
            "avg_us":3900,"max_us":8200,"over":0}]}]
```

`budget_us` 只是一個宣告值 —— 超過只記數，不會停掉你（停止與否由政策決定）。

**要量「直接執行」vs「被登記成任務」的差距**：`last_us` 是你的工作，`wrap_us` 是管理器
多做的（結束旗標、enable 判斷、兩次取時、計數、預算比較、延遲決策）。
所以「登記後的每輪成本 ≈ `last_us + wrap_us`」。

想看真實差距就同一個 `fn` 寫兩份：一份給任務管理器跑，一份用 `xTaskCreate` 直接跑，
固定秒數後比較 `count`（或反過來比較 CPU 時間）。`wrap_us` 的好處是不用改 code 就量得到，
而且它會隨 core / 頻率 / 編譯器變 —— 所以要在目標板、目標頻率下量。

---

## 5. 執行期控制

```c
nc_task_enable("my", false);     // 暫停：loop 還活著，只是不呼叫 fn
nc_task_enable("my", true);
nc_task_stop("my", 500);         // 停 loop（合作式，等它返回）
nc_task_start("my");             // 再開（on_start 不會重跑）
nc_task_set_core("my", 1);       // 換核：停 → 改 → 起
```

`kind=EFFECT` 的任務另外有：

```c
nc_task_attach(&eff_task);       // 登記 + on_start + 開 loop
nc_task_detach("fire26");        // 停 loop → on_stop → 收回指令與 provides
```

attach 的限制（都是刻意的）：

| 限制 | 為什麼 |
|---|---|
| 不能有 `mem[]` | plan 的宣告窗口在 commit 就關了；runtime 記憶體從父層 arena 切 |
| 不能是 `SYSTEM` | 系統層由韌體決定，不是由可替換的檔案決定 |
| 指令已被別人註冊 → 整個 attach 失敗 | 不要半套 |
| loop 停不下來 → detach 失敗 | 指標還活著就拆 = use-after-free |

---

## 6. 完整範例：`pulse`

```c
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nc_hub.h"
#include "nc_task.h"

static nc_hub_t *s_hub;
static volatile uint32_t s_ticks;

static void pulse_loop(void *arg)          // ③ 迴圈
{
    (void)arg;
    while (!nc_task_exit_requested()) {
        uint32_t v = ++s_ticks;
        uint32_t *slot = nc_hub_write_begin(s_hub);   // 沒空位就跳過，不等待
        if (slot != NULL) {
            *slot = v;
            nc_hub_write_commit(s_hub);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static int pulse_declare(nc_task_res_t *out, int max)   // ① 資源
{
    if (max < 1) return 0;
    out[0] = (nc_task_res_t){ .name = "pulse.hub", .bytes = 16 * sizeof(uint32_t),
                             .cls = NC_PLAN_INTERNAL, .align = 32 };
    return 1;
}

static esp_err_t pulse_start(const nc_task_res_view_t *res, int n)   // ② 拿資源
{
    (void)n;
    s_hub = nc_hub_create_in(res[0].ptr, res[0].len, "pulse",
                             sizeof(uint32_t), 16, 32);
    return s_hub != NULL ? ESP_OK : ESP_FAIL;
}

static void pulse_stop(void) { s_hub = NULL; }                   // ④ 收尾

static const nc_task_loop_t pulse_loops[] = {
    { .name = "pulse", .fn = pulse_loop, .stack = 3072, .prio = 3,
      .core = 0, .layer = 2 },
};

const nc_task_t pulse_task = {
    .name = "pulse", .kind = NC_TASK_NORMAL, .level = 2,
    .declare = pulse_declare,
    .on_start = pulse_start,
    .on_stop = pulse_stop,
    .loops = pulse_loops, .nloops = 1,
};
```

`main.c` 只加一行：

```c
nc_task_register(&pulse_task);
```

---

## 7. 檢查清單

- [ ] 名字全系統唯一（任務、資源、loop、provide 都算）
- [ ] loop 有看 `nc_task_exit_requested()`（否則管理器停不下來）
- [ ] `on_start()` 裡用參數給的 `res[]`，不要 `nc_plan_get()`
- [ ] `declare()` 裡沒有 `malloc`、沒有 `xTaskCreate`
- [ ] `on_start()` 失敗回錯誤，而且不留半啟動狀態
- [ ] 有東西要給別人就寫 `provides[]`，不要讓別人 include 你的 `.c`
- [ ] `period_ms` 有設就不要再自己 `vTaskDelay`（會變兩倍週期）

---

## 8. 目前限制

| 限制 | 影響 | 什麼時候補 |
|---|---|---|
| registry 沒有鎖 | attach/detach 與 status 同時發生時，status 可能少一個或重複一個（不會越界）。目前 attach 在開機、detach 在 fx task | 要從 decode task 直接載入 `.fxe` 時 |
| `nc_plan` 沒有 release | 停機不會還記憶體 | 需要「停用後重配」時 |
| 一個任務最多 2 個 loop、8 塊記憶體、8 項 provide | 目前都夠 | 真的不夠再放寬 |
| `on_stop` 只在「管理器停止」時呼叫 | `run_once` 自己結束的 loop 不會回呼 | 有需求時改成引用計數 |
