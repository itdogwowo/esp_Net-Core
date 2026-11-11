# 硬體門面 `nc_hw`：owner、command、snapshot

> 2026-11-10。**性質：第一版已上板；GPIO 已接，其他 driver 之後接。**
> 這一頁是「UI／實操／NC4 怎樣共用硬體」的契約。舊專案參考：
> `slave/lib/sys/hw_manager.py`、`slave/boot.py` Phase 1、`slave/driver/pin_drv.py`。
> 緩衝工具見 [buffers.md](buffers.md)；任務見 [task-model.md](task-model.md)。

---

## 1. 一句話

**中央只做目錄＋命令入口＋最新 snapshot；真正寫硬體的是 owner task。**

```text
NC4 action / UI
   │ nc_hw_send("gpio", cmd)       nc_hw_snapshot("gpio", ...)
   ▼                                      ▲
┌──────────────── nc_hw registry ────────────────┐
│ name → queue + snapshot（seqlock）             │
└───────────────────┬────────────────────────────┘
                    │ owner 取 command
                    ▼
             owner task（唯一 writer）
               · gpio_config / gpio_set_level
               · 更新 snapshot
```

不是舊 `hw_manager` 的「任何呼叫者都可以 `Pin.value()`」；
`nc_hw_send()` 只入 queue，真正的硬體操作在 owner。

---

## 2. 為什麼不是 runtime 仲裁

- 每個資源只有**一個 writer**；UI／NC4 只送 command，不直接碰 register。
- 多個 reader 讀同一份 snapshot，不需要 lock owner。
- 所以不需要舊式中央 manager 的 `HW.get/set` hot path；也不需要 runtime 的 pin 仲裁。
- 開機仍然要 **claim／validate**，因為 config 寫錯（兩條 bus 撞腳、踩 PSRAM）要在 init 前抓到。

---

## 3. API

```c
// 開機一次
esp_err_t nc_hw_init(void);

// owner 在 on_start 登記
esp_err_t nc_hw_register(const char *name);      // "gpio"

// 命令（任何 core）
esp_err_t nc_hw_send(const char *name, const nc_hw_cmd_t *cmd);
bool      nc_hw_recv(const char *name, nc_hw_cmd_t *cmd, uint32_t timeout_ms);

// 狀態（owner 寫、reader copy）
esp_err_t nc_hw_publish(const char *name, const void *data, size_t len);
size_t    nc_hw_snapshot(const char *name, void *out, size_t cap);

// 開機 claim
esp_err_t nc_hw_claim(int pin, const char *owner, const char *label, uint32_t flags);
bool      nc_hw_reserved(int pin);
bool      nc_hw_pin_claimed_by_other(int pin, const char *owner);
bool      nc_hw_claim_ok(void);
size_t    nc_hw_json(char *out, size_t cap);
```

- 上限：resource 32、command queue 8、snapshot 256 B、claim 64。
- command 是固定大小（16 B）；大資料（LED frame／音訊）用 `nc_buf`，不經這裡。
- snapshot 用 double buffer＋`seq`（seqlock）；reader copy 到自己的 buffer，不會拿到半份。

---

## 4. owner：`hw_gpio`

第一版 owner 是 `components/tasks/hw_gpio`：

- 登記 resource `"gpio"`。
- 命令：
  - `CFG`：pin、mode（0 out／1 in）、pull（0/1/2）、level。
  - `SET`：pin、level。
- 每 10 ms 採樣已配置的 input pin，更新 snapshot 並 publish。
- 保留腳位（S3 26–37、19/20、43/44、0/3/45/46）直接拒絕。
- 已被 SD／ETH claim 的腳位也拒絕（`nc_hw_pin_claimed_by_other`）。
- 心跳：每輪 `nc_task_beat()`；避免長跑 loop 被誤判停滯。

owner 是 `NC_TASK_SYSTEM`、level 2、core 0、priority 6（`NC_PRIO_OTHERS_MAX`）。
之後的 pixel／audio／display 各自是 owner，不共用同一個 driver。

---

## 5. 開機 claim（`ph_hwcheck`）

`main.c` 在 `storage+cfg` 之後、`hardware` 之前新增：

```text
ph_storage    掛載 + nc_cfg_boot()
ph_hwcheck    nc_hw_init() → 由 typed config 收集 ETH／SDIO 腳位 → claim → validate
ph_hardware   ETH／SDIO 真正 init
```

第一版只 claim 已型別化的 `hardware.json` 段（ETH、SDIO）：
SDIO 的 clk／cmd／D0–D3；ETH 的 mdc／mdio／ref_clk。
其他段（PIN／SPI／I2C／I80／UART／I2S／PWM／ENC／XL9555）目前仍是 `unknown`，
等 driver 有型別才逐個加 claim。

`nc_hw_json()` 印 `resources` 與 `claims`；衝突時 `nc_hw_claim_ok()=false` 並印 `ESP_LOGE`。
第一版仍然繼續開機（fail-visible，不 halt）；`boot_strict` 之後再加。

---

## 6. 測試指令與上板結果

| Cmd | 用途 |
|---|---|
| `0xF035 HW_GPIO` | `[op][pin][mode][pull][level]`：0=CFG、1=SET；只入 queue，回 `queued` |
| `0xF036 HW_SNAP` | 讀 `"gpio"` snapshot：`{"levels":…,"low":…,"high":…}` |
| `0xF037 HW_INFO` | resource／claim 表 |
| `0xF025–0xF027` | 舊的直接 GPIO 測試（保留作 raw 逃生門；**不經 nc_hw**） |

**2026-11-10 S3 上板**：

```json
{"resources":{"gpio":0},"claims":[
  {"pin":7,"owner":"sdio","label":"clk","flags":2},
  {"pin":6,"owner":"sdio","label":"cmd","flags":2},
  {"pin":15,"owner":"sdio","label":"data","flags":2},
  {"pin":16,"owner":"sdio","label":"data","flags":2},
  {"pin":4,"owner":"sdio","label":"data","flags":2},
  {"pin":5,"owner":"sdio","label":"data","flags":2}]}
```

- CFG pin 21 output high → snapshot `levels=2097152`（bit21）。
- SET pin 21 low → `levels=0`。
- SET pin 7（SD clk，已 claim）→ **被拒**，snapshot 不變。
- `hw_gpio` heartbeat 正常（`b` 持續增加），loop 沒有被誤判停滯。
- hosttest 全部通過；`nc4_test` 9/9。

---

## 7. UI／實操／NC4 的用法

```text
control owner（core0）
  · 唯一 writer
  · 10 ms 採樣 → snapshot
  · 安全點套用 command

UI（core1）
  · 讀 snapshot（10–30 Hz）
  · 改設定 → nc_hw_send(CONFIG_APPLY)
  · 不直接 gpio_set / i2c_write

NC4 action
  · nc_hw_send("gpio", {SET,...})
  · 回 queued；要結果就查 snapshot
  · 不在 decode handler 阻塞等 owner
```

設定變更：
- UI 只改 draft；config owner 驗證、暫停受影響 owner、套用、`generation++`、ack／rollback。
- 只有一個 config writer；不要兩個 core 同時寫 `/data/config`。

---

## 8. 現況與未做

| 項目 | 狀態 |
|---|---|
| `nc_hw` registry／queue／snapshot／claim | ✅ 第一版 |
| `hw_gpio` owner | ✅ 上板 |
| `ph_hwcheck`（ETH／SDIO） | ✅ 第一版 |
| 全部 hardware.json 段 claim | ⏸ 等 driver 型別化 |
| UI／config owner | ⏸ 未接 |
| pixel／audio owner | ⏸ 下一步 |
| 舊 `0xF025–27` 改走 nc_hw | ⏸ 保留 raw 測試 |
| board／wiring／user config 拆分 | ✅ 第一版：wiring 為 build-time base，user hardware 只寫 delta；`_chips` profile 表已建立，runtime merge 未做 |

**不要**把 `nc_hw` 當成新的 `hw_manager`：
它沒有 `HW.get()` 的即時硬體讀取，也沒有一堆 driver 的直接指標；
它只是一張目錄、每資源一條 queue、一份 snapshot。
