# 任務資源：怎樣宣告、怎樣配對、怎樣提交

> 回答三個問題：**任務要什麼（declare）→ 同邊個配對（match）→ 交咩出去（provide）**。
> 相關：[hw-ownership](hw-ownership.md)、[execution-rules](execution-rules.md)、
> [nc_res.h](../../firmware/components/host/ncm/include/nc_res.h)、
> [nc_task.h](../../firmware/components/sys/task/include/nc_task.h)。

---

## 1. 四類「資源」

| 類型 | 誰用 | 宣告 | 配對 | 提交／使用 |
|---|---|---|---|---|
| **記憶體** | 固件任務 | `nc_task_t.mem[]` 或 `declare()` | `nc_plan` 開機一次 carve（class／align／bytes） | `on_start(res_view)` 收 `ptr/len/buf` |
| **硬體接線 claim** | 固件任務 | `nc_task_t.claims[]`（`"pin:16"`、`"bus:spi1"`、`exclusive`） | `audit()` 檢查獨占衝突 | 用該硬體的 owner API（例如 `nc_hw_send("gpio", …)`） |
| **provide（對外提供）** | 固件任務 | `nc_task_t.provides[]`（buf／json／u32） | 全域 provides 表，名字唯一 | 提供 callback；別人用 `nc_provide_*()` 讀 |
| **外掛資源** | `.nct`／`.nce` | meta 的 `requires`／`provides` | `resources.json` 的 provider 表 ＋ `nc_res_check()` | 載入前擋；通過先 attach |

---

## 2. 記憶體：declare → plan → on_start

```c
static int my_declare(nc_task_res_t *out, int max)
{
    if (max < 1) return 0;
    out[0] = (nc_task_res_t){
        .name = "mytask.rx", .bytes = 4096, .cls = NC_PLAN_INTERNAL, .align = 32,
        // 可選：slot_bytes/n 讓管理器直接建成 nc_buf
        .slot_bytes = 256, .n = 16, .flags = NC_BUF_F_STREAM,
    };
    return 1;
}

static esp_err_t my_start(const nc_task_res_view_t *res, int n)
{
    void *p = res[0].ptr;       // 由 plan carve 出嚟，開機用到關機
    struct nc_buf *b = res[0].buf;   // slot_bytes>0 時已建好
    ...
}

static const nc_task_t my_task = {
    .name = "mytask", .kind = NC_TASK_NORMAL, .level = 2,
    .declare = my_declare, .on_start = my_start,
};
```

規則：熱路徑唔准 `malloc`；宣告窗口喺 `nc_task_declare_all()` 一次過關；宣告值係
**上限**，唔係每次重配。

---

## 3. 硬體接線 claim：宣告 → audit → 用 owner

```c
static const nc_task_claim_t my_claims[] = {
    { .id = "pin:21", .exclusive = true },   // 我要獨占 GPIO21
    { .id = "bus:spi1", .exclusive = false },// 共用 SPI1
};
static const nc_task_t my_task = {
    .name = "mytask", .kind = NC_TASK_NORMAL,
    .claims = my_claims, .nclaims = 2,
};
```

- 配對喺 `nc_task_declare_all()` 之後的 `audit()`：同一條 `id` 如果兩邊都
  `exclusive`，**後者那條 claim 不算**（記在 `claim_off` bitmap，`nclaims` 不改；
  以前直接減數會漏掉後面的衝突）。
- 衝突只影響「claim 有效性」，唔會令任務唔啟動；要係邏輯上拒絕，自己查
  `nc_hw_pin_claimed_by_other()`。
- 真正寫硬體只可以經 owner（見 §6）。

---

## 4. provide：提交一個「別人可讀」的資源

```c
static bool prov_frame_buf(void **ptr, size_t *bytes)
{
    if (!running) return false;         // false = 目前沒有
    *ptr = s_fb; *bytes = sizeof(s_fb);
    return true;
}
static size_t prov_stats_json(char *out, size_t cap) { ... }

static const nc_task_provide_t my_provides[] = {
    { .name = "pixel.out",   .kind = NC_PROVIDE_BUF,  .desc = "輸出 frame", .buf = prov_frame_buf },
    { .name = "pixel.stats", .kind = NC_PROVIDE_JSON, .desc = "統計",       .json = prov_stats_json },
};
```

- 任務 `on_start` 後登記；`on_stop`／卸載時收回（`nc_provide_owner()` 會變 NULL）。
- 名字全系統唯一；`STATUS_GET ?provides` 可列出。
- 讀者最多用：

```c
void *p; size_t n;
if (nc_provide_buf("pixel.out", &p, &n)) { /* 只讀，唔好寫 */ }
```

---

## 5. 外掛模組的資源：manifest ＋ resources.json

外掛用 meta 宣告（`ncm.py pack --meta meta.json`）：

```json
{
  "requires": [
    {"type": "hw",      "name": "gpio", "exclusive": true},
    {"type": "buf",     "name": "led.frame", "bytes": 2704}
  ],
  "provides": [
    {"type": "provide", "name": "hello.heartbeat"}
  ]
}
```

`resources.json`（用戶可調）宣告「邊個提供咩」：

```json
{
  "resources": [
    { "name": "led.frame", "type": "buf", "provider": "pixel", "bytes": 2704 },
    { "name": "gpio",      "type": "hw",  "provider": "hw_gpio" }
  ]
}
```

開機配對順序：

```
B06  nc_res_build()        ← resources.json → prov 表
     nc_mod_declare_res()  ← 模組 meta provides/requires 併入
     nc_res_merge()        ← 標 present（provider 名唔為空）
B08  nc_res_check(name)    ← 逐條 requires 檢查：存在？provider 起咗？bytes 夠？
                               有 exclusive 衝突？任何一項唔過 → 拒絕載入該模組
```

**配對規則**

| 檢查 | 失敗結果 |
|---|---|
| `requires` 的名字有冇 provider | 拒絕載入（`缺少 xxx`） |
| provider 任務有冇啟動 | 拒絕載入（`provider 未啟動`） |
| 要求 bytes ≤ 提供 bytes | 拒絕載入 |
| `exclusive` 同其他 owner 撞 | 拒絕載入 |
| meta 重複／超出上限（8 條） | 掃描時拒收該模組 |

---

## 6. GPIO 具體例子（唯一 writer 契約）

```
wiring.json ──開機──> nc_hw_claim(pin, owner, label, flags)   ← 一次過搶腳位
                         │ 衝突：boot 直接 fail（s_ok=false）
hw_gpio owner task ──> nc_hw_register("gpio")
                         ├─ recv nc_hw_cmd_t {CFG/SET}   ← 唯一寫 gpio_set_level
                         ├─ publish nc_hw_gpio_snap_t    ← levels/mode
                         └─ 別人讀 nc_hw_snapshot("gpio", &snap, sizeof)
UI／NC4 action ──> nc_hw_send("gpio", &cmd)
```

- 「某些狀態」（方向／pull／默認電平）＝ `CFG` command 一次過交：
  `pin`、`arg`=mode、`data[0]`=pull、`data[1]`=level。
- 想由模組指定狀態 → 先要 capability：manifest `requires: [{type:"hw",
  name:"pin:21", exclusive:true}]`，由 loader 交 `nc_hw_claim()`；再開 ABI
  `nc_gpio_cfg(pin, mode, pull, level)`。
- **現況缺口**（T-NCM-019）：模組 `nc_gpio_*` 直接 `gpio_config/set_level`，
  繞過 owner、唔入 snapshot、冇 claim → 暫時只可以當 demo，未算契約一部分。

---

## 7. 開發 checklist

- [ ] 記憶體用 `declare()`，唔喺 loop 入面 alloc
- [ ] 獨占腳位／bus 寫入 `claims[]`，唔好靠「冇人同我爭」
- [ ] 對外資料用 `provides[]`（名字全域唯一），唔好直接 export 全域變數
- [ ] 外掛：`requires` 寫齊、`resources.json` 有 provider、`ncm.py pack --meta` 嵌入
- [ ] 硬件操作一律經 owner（`nc_hw_send`），唔好喺任務／action 直接寫 register
- [ ] `STATUS_GET` 睇得到你嘅 provide／任務狀態
