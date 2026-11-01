# NC4 收／解路徑 — 正式架構（RX path）

> **狀態**：規劃（機制已驗證，尚未收進架構）
> **參考框架**：`mp_Net-Core` — `bus_sources` → `BusDecodeTask` → `handle_stream`
> **相關**：[testing.md §2.13/§2.14](../02_guides/testing.md)（量測）、
> [buffer-manager.md](buffer-manager.md)（記憶體）、[component-layout.md](component-layout.md)（分區）、
> [architecture-flow.md](architecture-flow.md)（執行期流程）
> **最後更新**：2026-11-02

---

## 1. 一句話

**「分核收 + 零複製解析」已經被證明可行；現在要把它從 `main.c` 的測試程式，
收成 mp_Net-Core 那套「傳輸總線 → 解碼層 → 執行」三層，而且解碼層是「實例」——
可以隨時換核、也可以在另一個核心再建一個。**

---

## 2. 對照 mp_Net-Core（同一套骨架，換語言）

| mp_Net-Core（參考） | 這裡的目標（C） | 現在在哪 |
|---|---|---|
| `bus_sources`（傳輸總線註冊表） | `nc4_bus_registry` — **只存 ID**（1 byte），bus 描述子放在固定 pool | ❌ 還沒有（`s_buses[]` 靜態陣列） |
| 每條 bus 的 `rx_hub`（AtomicStreamHub） | bus 自己的 slot ring（8 格） | ✅ `nc4_bus.c` 的 `slots[]` |
| `rx_hub.get_read_view()` / `release_read()` | `nc4_bus_frame_begin()` / `nc4_bus_frame_end()` | ✅ 已有（零複製） |
| `BusDecodeTask`（Task 實例，可 `set_affinity`） | `nc4_decode_t`（引擎實例，`core` 是參數；**掛的是 bus ID 不是物件**） | ❌ 現在是「每個核一個寫死的 task + `cfg.core` 過濾」 |
| `_parsers[id(bus)]`（每 bus 一個 parser） | bus 內建 parser（PULL 用 ring，frame 模式無狀態） | ✅ 已有 |
| `app.handle_stream()` → 指令表 | `nc4_parser_frame()` → `nc4_decoder_t` 表 → handler | ✅ 已有 |
| `_TxOut`（每條 bus 的發射出口） | `nc4_bus_cfg_t.send` / `send_arg` | ✅ 已有 |
| `decode_budget_slots`（每輪上限） | `budget_per_pass` | ✅ 已有 |
| TaskManager 分層（bus_decode 在 layer 1） | `nc_task_register` 的 `layer` | ✅ 已有（但 bus/decode 還沒用它） |

**結論：資料面（hub、零複製、parser、dispatch、發射）都已經有了；
缺的是「總線註冊表」與「解碼引擎實例」這兩個結構，其餘都是把它們接起來。**

> **與參考的一個刻意差異**：mp 版只有**一個** `BusDecodeTask`，它每 100 ms 從
> `bus_sources` 重新抓「全部」bus（所以兩顆核心不能各跑一個，會重複解同一幀）。
> 這裡要「分核／可重複」，所以引擎用**明確 attach** 分區；
> 只有單引擎的情況才用 `attach_all`（等於參考版的行為）。

---

## 3. 三層責任（照你的說法）

```
① 傳輸總線（transport bus）
     一條管線 = 一個 nc4_bus handle
     責任：把 socket/USB/UART 的位元組變成「一格一整幀」（或一段位元組）放進 rx_hub
     知道：socket、連線、重連、frame 邊界
     不知道：parser、handler、hub 的交換邏輯

                │  nc4_bus_registry_add(cfg) → id   ← 註冊只給描述子，不含緩衝
                ▼
② 解碼層（decode engine）＝ 實例，不是寫死的 task
     nc4_decode_t { core, prio, stack, budget, bus_ids[], stats }
     責任：輪詢自己掛的 bus → 取幀 → parse → dispatch；有資料就繼續、沒事就等 kick
     **歸屬**：每條 bus 自己記「我歸哪個解碼器」（cfg.decoder，預設 0 = core0）。
               解碼器啟動時才掃出「歸我的」bus，**這時候才建它的緩衝（slots + parser）**；
               歸 core1 的 bus 只有真的指定了才會讓 core1 的解碼器去配緩衝。
     可調整：放在哪一核、掛幾條 bus、每輪預算
     可重複：core0 一個引擎、core1 一個引擎，各自掛不同的 bus

                ▼
③ 執行（dispatch / action）
     nc4_decoder_t 指令表 → handler（同步）或 job hub → slow_worker（慢工作）
```

**關鍵差異（相對現在的程式）**：bus 不再檢查「我屬於哪一核」；是**引擎去掛 bus**。
所以換核 = 把 bus 掛到另一個引擎（或把引擎搬到另一核重啟），不用改 bus 的程式。

---

## 4. 目標結構

### 4.1 檔案

```text
components/sys/
  nc4/            nc4.c（framing/parser/dispatch）
                  nc4_bus.c（bus + slots + frame_begin/end + kick + stats）
                  nc4_decode.c（★ 新增：引擎實例 + registry）
  transport/      tcp_nc4.c（★ 從 main.c 抽出）、cdc_nc4.c（S3）、uart_nc4.c
  config/         config.json → nc_net_cfg_t
components/sys/tasks/
  net_task.c      （可選）用 nc_task 註冊引擎，取代 main.c 直接 xTaskCreate
main/main.c       ← 只剩裝配：declare → commit → registry_add → decode_start
```

### 4.2 API（新）— 全部用 ID，物件不進陣列

```c
typedef uint8_t nc4_bus_id_t;      // 0 = 無效
typedef uint8_t nc4_dec_id_t;      // 0 = 無效

// ── ① 傳輸總線（描述子 pool + ID 註冊表）────────────────────
// add 只做三件事：佔一個描述子、填 cfg、把 ID 放進註冊表。
// **不建緩衝**（slots/parser 由解碼器在 start 時建）。
nc4_bus_id_t nc4_bus_registry_add(const nc4_bus_cfg_t *cfg);
esp_err_t    nc4_bus_registry_remove(nc4_bus_id_t id);
nc4_bus_t   *nc4_bus_get(nc4_bus_id_t id);        // 內部查表用
size_t       nc4_bus_registry_json(char *buf, size_t cap);

// bus 自己記「我歸誰」；預設 0（core0 的解碼器）
typedef struct {
    ...
    nc4_dec_id_t decoder;      // 0 = 預設解碼器（core0）
    int    slots;              // 這條 bus 幾格（每條可以不同）
    size_t slot_size;          // 每格多大（依這條線的最大幀）
    ...
} nc4_bus_cfg_t;

// ── ② 解碼引擎（實例）─────────────────────────────────────
typedef struct {
    const char *name;          // "decode0" / "decode1"
    int core;                  // 0 / 1（-1 = 不指定）
    UBaseType_t prio;
    uint32_t stack;
    uint32_t budget;           // 每輪最多處理幾幀（0 = 8）
    uint32_t idle_ms;          // 沒資料時等 kick 的上限（0 = 10）
} nc4_decode_cfg_t;

nc4_dec_id_t nc4_decode_create(const nc4_decode_cfg_t *cfg);   // 只建引擎（不配緩衝）
esp_err_t    nc4_decode_start(nc4_dec_id_t id);   // ★ 掃「歸我的 bus」→ 建 slots/parser → 起 task
esp_err_t    nc4_decode_stop(nc4_dec_id_t id);    // 停 task → 釋放它配的緩衝
esp_err_t    nc4_decode_move(nc4_dec_id_t id, int core);       // stop → core → start
size_t       nc4_decode_json(nc4_dec_id_t id, char *buf, size_t cap);
```

**歸屬與預設**：`cfg.decoder` 沒填 = 0（core0 解碼器）。
只有明確寫 `decoder = 1` 的 bus，才會讓 core1 的解碼器在 `start` 時替它配緩衝；
core0 的解碼器連看都不看它。反過來，core1 沒有被指定任何 bus 時，它不配任何記憶體。

「重複建立在不同核心」就是：

```c
nc4_bus_id_t tcp  = nc4_bus_registry_add(&(nc4_bus_cfg_t){
    .name = "TCP", .mode = NC4_BUS_PUSH, .decoder = 0,
    .slots = 4, .slot_size = 8205 });

nc4_bus_id_t uart = nc4_bus_registry_add(&(nc4_bus_cfg_t){
    .name = "UART", .mode = NC4_BUS_PULL, .decoder = 1,
    .chunk = 512 });

nc4_dec_id_t e0 = nc4_decode_create(&(nc4_decode_cfg_t){ .name = "decode0", .core = 0 });
nc4_dec_id_t e1 = nc4_decode_create(&(nc4_decode_cfg_t){ .name = "decode1", .core = 1 });
nc4_decode_start(e0);   // 只配 TCP 的 slots
nc4_decode_start(e1);   // 只配 UART 的（PULL 只有 chunk）
```

同一核跑兩個引擎也可以；SPSC 是 per-bus，不會衝突。
引擎之間互不知道對方存在，也不共用任何緩衝。

### 4.3 數量：不設人為上限

**決定：不加 `NC4_MAX_BUSES` 這種編譯期上限，也不設 slot 預算。**
註冊表、引擎的 bus 清單、描述子全部**動態配置**；配不到就是那一條失敗，不是整批、也不是崩潰。

| 項目 | 做法 | 失敗時 |
|---|---|---|
| 註冊表 | 動態陣列（`realloc`，元素是 `uint8_t` ID） | 回 `ESP_ERR_NO_MEM`，不影響已註冊的 |
| 描述子 | 每條 bus `calloc(1, sizeof(nc4_bus_t))`（≈200 B） | 同上 |
| slot 記憶體 | 解碼器 `start` 時 `heap_caps_malloc`（或 plan） | 該 bus 標記 `failed`，引擎跳過它，其他照跑 |
| 解碼引擎 | 動態 | 回 `ESP_ERR_NO_MEM` |

**唯一剩下的限制是 heap 本身**，而且它會用「哪一條 bus、要多少、差多少」的形式回報，
不是靜默或崩潰：

```json
"buses": [
  { "name": "TCP",  "bytes": 32820, "ok": 1 },
  { "name": "TCP2", "bytes": 32820, "ok": 1 },
  { "name": "TCP5", "bytes": 65640, "ok": 0, "err": "no mem (free 41200)" }
]
```

風險與對策（誠實寫在這裡）：

- **碎片化**：描述子小、開機時一次配完；大塊（slots）每條一次配完，不逐幀配。
- **配不到的線**：`ok:0` 留在 registry 裡，`decode_start` 會再試一次（例如先停掉一條再啟動）。
- **不要用「先開 20 條 8K 線」當設定**：那不是限制問題，是算術問題（20 × 65,640 B = 1.25 MB）。

> 3–4 條是常態；不設上限代表「程式不擋你」，但 heap 會擋，而且會告訴你擋在哪。

### 4.4 生命週期與分層
|---|---|---|
| L0 | transport（tcp_nc4 / cdc_nc4） | 它自己 `on_start` 時建 bus + registry_add |
| **L1** | 解碼引擎 | **必須晚於 L0** —— 啟動時 registry 裡已經有 bus |
| L2 | 慢工作 worker（jobs） | 解碼引擎之後 |

**這正是 mp_Net-Core 的教訓**（`Core_Manager.py` 的註解）：`bus_decode` 放 layer 1，
不跟「產生通道」的任務同層，否則啟動時通道還沒註冊。這裡用 `nc_task` 的 layer 直接解掉。

**L1 的啟動順序（容易踩）**：解碼器 `start` 對每條歸它的 bus 依序做
①配 slots/parser → ②呼叫 bus 的 `ops->start()`（transport 這時候才起 RX task / accept）。
順序顛倒就會出現「RX 已經在收，但插槽還沒配」的 NULL 解參考。

---

## 5. 設定（config.json）

```json
"Network": {
  "lan": { "...": "既有" },
  "nc4": {
    "links": [
      { "name": "TCP", "kind": "tcp", "port": 6000,
        "mode": "push_frame", "decoder": 0,
        "slots": 4, "slot_size": 8205, "stall_timeout_ms": 2000 },
      { "name": "TCP2", "kind": "tcp", "port": 6001,
        "mode": "push_frame", "decoder": 1,
        "slots": 4, "slot_size": 8205 },
      { "name": "CDC", "kind": "cdc",
        "mode": "pull", "decoder": 0, "chunk": 1024 }
    ],
    "decode": [
      { "name": "decode0", "core": 0, "prio": 4, "budget": 8 },
      { "name": "decode1", "core": 1, "prio": 4, "budget": 8 }
    ]
  }
}
```

- **`links` 是陣列，不限條數**：想開幾個 port 就寫幾個；程式不設上限，配不到記憶體的那一條
  會標記失敗（見 §4.3）。沒寫 `decoder` 就是 0（core0）。
- `mode`：`pull` / `push_slot` / `push_frame`。
- `slots` / `slot_size` 每條可以不同（大幀線少格、小幀線多格）。
- 讀不到／欄位缺 → 程式預設（config 壞掉不能讓收包停擺）。
- `0xF00A` 保留為 **debug override**（寫 NVS，優先於 config）。

---

## 6. 記憶體（buf 由「擁有它的解碼器」配，走 plan）

**順序**：transport 先註冊描述子（不配緩衝）→ 解碼器 `start` 時才為自己的 bus 配 slots。

```c
// 每個 bus 一項宣告；名字帶 bus 名，失敗才看得出是哪一條
nc_plan_declare(&(nc_plan_decl_t){
    .service = "net", .name = "net.TCP.slots", .level = 1,
    .cls = NC_PLAN_INTERNAL, .bytes = cfg.slots * cfg.slot_size, .align = 32,
});
```

- `nc4_bus_cfg_t` 加 `slots_region / slots_bytes`；有就用 plan 的，沒有才 `calloc`（host 測試）。
- 一項配不到 → **那一條 bus 標記失敗**，其他照跑（與 plan 的逐項失敗一致）。
- `decode_stop` 釋放它配的緩衝；`decode_move` = stop → start（重配）。

驗收：plan 報表出現 `net.TCP.slots`、`copied=0`、heap 不掉、`nc4_bus_registry_json` 看得到歸屬。

---

## 7. 狀態

`0x1101 STATUS keys=net`：

```json
"net": {
  "buses": [ { "name": "TCP", "mode": "push_frame", "frames": 2048, "mb": 16.0,
               "copied": 0, "us_per_frame": 414.8, "slots": [6,0,1,1] } ],
  "decode": [ { "name": "decode0", "core": 0, "running": 1, "budget": 8,
                "passes": 12345, "idle_ms": 10 } ]
}
```

`0xF008` 保留成測試別名（同一份 JSON）。

---

## 8. 錯誤與恢復政策（正式化）

1. **錯誤一律收線**：`recv/send` 非 EAGAIN → close + fd=-1（只回錯誤碼會讓 accept 卡死）。
2. **新連線 = 新位元組流**：accept 後、交 fd 之前 reset parser + 清 READY 插槽。
3. **閒著 ≠ 壞線**：沒有半個幀可以無限等；有半個表頭／半幀才啟動 `stall_timeout_ms`。

---

## 9. 不變量

1. 一條 bus 只有一個 producer、一個 consumer（SPSC）。
2. frame 模式：`slot_size ≥ NC4_MAX_FRAME`，producer 只能 commit 整幀。
3. API 不等待、不丟已 commit 的資料；沒有空位回 NULL。
4. 等資料用 `select()`；等插槽用 `taskYIELD()`／notification；**不准 `vTaskDelay(1)` 當退避**。
5. commit 後必須 `nc4_bus_kick()`（延遲 = 輪詢週期）。
6. handler 不得持有 `frame.data`（`frame_end` 後就還回去）。
7. **引擎不擁有 bus**：`decode_stop` 不能關掉 bus；bus 移除時引擎要能安全跳過。
8. **誰配誰釋放**：slots/parser 由「擁有這條 bus 的解碼器」在 `start` 配置、`stop` 釋放；
   transport 只給描述子。歸 core1 的 bus，core0 的解碼器不得碰它的緩衝。

---

## 10. 遷移計劃

| 階段 | 做什麼 | 驗收 |
|---|---|---|
| **P0** | 新增 `nc4_decode.c`：ID 註冊表 + 引擎實例 + **解碼器啟動時才配緩衝**；行為不變（一個引擎 per core） | `bench_all.py` 全表與 §2.13 一致；三種模式都能跑；`bus_registry` 只有 ID |
| **P1** | `sys/transport/tcp_nc4.c`：TCP server / RX / 錯誤政策從 `main.c` 抽出 | `main.c` 只剩裝配；數字不變 |
| **P2** | `config.json` → `nc_net_cfg_t`（mode/core/slots/slot_size/decoder）；`0xF00A` 降級 | 改 config 重開機生效；拔掉 config 用預設 |
| **P3** | slots 走動態配置（plan 可選）+ 逐條失敗回報 | 配不到只讓那一條 `ok:0`；registry JSON 看得到 bytes/err |
| **P4** | `0x1101 keys=net` + `nc_task` 分層（transport L0 / 引擎 L1） | 正式 status 看得到歸屬與 bus 清單 |
| **P5** | S3 實機（CDC PULL）+ 10 分鐘 soak | 兩塊板都穩 |
| **P6** | **slot 數／大小調參**：`slots ∈ {2,4,8}` × `slot_size ∈ {1K,4K,8K}` 各跑 16 MB，看吞吐／`copied`／`us_per_frame`／記憶體 | 產出一張「哪種線用幾格多大」的表，寫回本文件 |
| ~~**P7**~~ | ✅ **WS 第一版已完成**（`components/sys/transport/ws_nc4.c`）：訊息即一幀、直接在 httpd buffer 上解析、`0x1101` 可查 | 10 項測試全過；吞吐 8K 4.05 MB/s（見 testing §2.15） |

P0 是純結構重構：**不改任何行為、不改任何數字**。跑掉就退回。

## 11. 服務管理器（service manager）＋ 被動資源管理器

### 11.1 角色只有兩個

```
   ┌─────────────────────────┐          ┌──────────────────────────┐
   │ 服務管理器（主動）        │  取/還   │ 資源管理器（被動）         │
   │ · 服務的註冊 / 分層 / 啟動│ ───────► │ · 只知道「我手上有什麼」    │
   │ · 代服務向資源管理器取得   │          │ · 一個接口：declare/      │
   │ · 啟動失敗逐項回報        │ ◄─────── │   acquire / release / 查詢 │
   └─────────────────────────┘          └──────────────────────────┘
```

**服務只「宣告需求」，從來不「取用」。**
需求可以是一張靜態表，也可以是一個 `plan()` 回呼（讀 config 算出來的），
但**取用永遠是服務管理器做的** —— 這一點沒有第二條路。

### 11.2 服務長什麼樣

```c
typedef struct {
    const char *name;                 // "nc4.tcp"
    int level;                        // 啟動層（0 = 系統必需，先啟動）

    // ① 宣告：讀 config 決定要多少（開機一次，commit 之前）
    //    靜態需求就回固定表；動態（N 條 bus）就在這裡算出來。
    int (*plan)(const nc_cfg_t *cfg, nc_res_req_t *out, int max);

    // ② 啟動：資源指標由管理器交進來（不是服務自己去 get）
    esp_err_t (*start)(const nc_res_view_t *res);

    // ③ 停止：管理器在 stop 之後才 release
    void (*stop)(void);
} nc_service_t;

esp_err_t nc_service_register(const nc_service_t *svc);   // 只登記
```

### 11.3 開機順序（服務管理器驅動）

```
config.json 載入
   │
   ├─ ① 每個服務的 plan()  →  資源管理器 declare（窗口在 commit 關閉）
   │
   ├─ ② 資源管理器 commit  →  每個 class 只 carve 一次（逐項 ok/fail）
   │
   ├─ ③ 依 level 逐服務 start：
   │       服務管理器 acquire 這個服務的每一項 → 全拿到才呼叫 start(res)
   │       任何一項失敗 → 這個服務不啟動（回報「哪一項、差多少」），其他照跑
   │
   └─ ④ stop 時：先 stop()，再 release（反向）
```

- **系統必需**（log、status、任務表…）就是 `level = 0` 的服務，**先宣告、先取得**；
  後面的服務吃不到它的額度。
- **動態數量**（config 決定 N 條 TCP bus）不是例外：`plan()` 在開機讀 config 時就算好
  `N × slots × slot_size`，一樣在 commit 之前宣告。**執行期不需要再取**。
- 真的要在執行期新增 bus（config 沒有的），才由擁有者跟資源管理器要 —— 那是**例外路徑**，
  而且要記在服務的 status 裡；正常開機流程不走這條。

### 11.4 跟現在的程式差在哪

| 現在 | 目標 |
|---|---|
| 各服務自己在 `svc_*_start()` 裡 `nc_plan_get()` | 服務管理器代取，透過 `start(res)` 交進來 |
| 資源用字串 name 查 | 資源管理器給 ID；服務拿到的是 view（指標＋長度） |
| 只有 get，沒有 release | `release()` + 反向歸還 |
| 沒有「系統必需」的特權 | level 0 先宣告、先取得 |
| `nc_task` 只管排程 | 服務管理器 = 排程 + 資源交付（同一個地方） |

### 11.5 效能

**零影響。** declare / commit / acquire / release 全部在開機與停機路徑；
迴圈裡沒有任何多一層。現在 decode 迴圈唯一的固定成本是 2 次 `esp_timer_get_time()`
（統計用）≈ 0.6 µs，對比解析 415 µs = **0.14%**。

> 一句話：**一個主動的服務管理器，一個被動的資源管理器；服務只宣告，管理器負責取。**

## 12. 明確不做

- **加密**：目前只有 CRC32（完整性），沒有加密；要加密是另一個主題。
- **多連線**：TCP 一次一條。要多條得先決定「多 bus」還是「一 bus 多 stream」。
- **單串流 10 MB/s**：65 KB 視窗 / 8.7 ms RTT 下不可能；要動視窗與 lwIP 池。
- **跨核熱遷移**：`nc4_decode_move()` 是 stop→restart，不做「執行中搬家」。
