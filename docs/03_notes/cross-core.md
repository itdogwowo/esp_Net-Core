# 跨核心：邊啲中斷會打到另一個核

> **用途**：把所有「一個核做嘢 → 另一個核即刻被叫醒／被凍結」的路徑逐條列出來。
> 呢份係**現況證據**（事實＋位置）；要求／不變式喺 [core-isolation.md](core-isolation.md)，
> 設計取捨喺 [execution-model.md](execution-model.md)。
>
> 平台：ESP32-P4 / ESP32-S3，`CONFIG_FREERTOS_NUMBER_OF_CORES=2`、
> `CONFIG_FREERTOS_HZ=1000`、`CONFIG_FREERTOS_SMP=y`（FreeRTOS-Kernel-SMP）。
> IDF v6.0.3（`~/esp/esp-idf`）。

---

## 0. 結論先講

跨核影響有四類，**唔好當成一樣**：

| # | 機制 | 另一個核即刻做咩 | 打中斷？ | 成本 |
|---|---|---|---|---|
| 1 | **FreeRTOS cross-core yield** | 收 `REASON_YIELD` 中斷 → 入 ISR → 重新排程 | ✅ | 幾 µs |
| 2 | **esp_ipc** | 叫醒該核的 `ipc_task`（優先級 24）跑指定 fn | ✅（用 notify 叫醒） | 幾 µs ~ 幾十 µs |
| 3 | **flash 快取同步** | 被叫去 `spi_flash_op_block_func`：**關快取、關非 IRAM 中斷、自旋等** | ✅（IPC）＋之後**完全凍結** | 1.6 ms ~ 36 ms |
| 4 | **mutex / spinlock** | 睡（mutex）或自旋（spinlock） | ❌ | 排隊時間，可能好長 |

> 真正「中斷」只有 1、2、3；4 唔會打中斷，但佢造成嘅延遲往往比中斷大幾個數量級。
> 第 3 類係本專案最貴嘅一種：**寫 flash 期間另一個核冇得快取**，等於暫時單核。

---

## 1. 機制層（IDF 內部，唔係我們寫的）

### 1.1 cross-core yield（`esp_crosscore_int_send_yield`）

- 實作：`esp_system/crosscore_int.c`（`REASON_YIELD` → 對方核 ISR → `vPortYieldFromISR`）。
- 由 FreeRTOS SMP 核心決定幾時發：`tasks.c` 的 `prvYieldCore()` → `portYIELD_CORE()` → `vPortYieldCore()`。
- 判斷邏輯（`prvYieldForTask()`，`tasks.c:882`）：
  - 一條任務被喚醒（queue／semaphore／notify／逾時）之後，
    揀一個「正在跑、優先級**低於**它、而且 affinity 允許」的核；
  - 如果嗰個核**唔係自己** → 發 cross-core yield（`tasks.c:882`）。
  - 對方核正在跑**同級或更高**優先級任務 → **唔會**打斷，只入 ready list，
    等對方核自己讓出（或者下一個 tick）。
- 其他會發 yield 的情況：
  - `vTaskDelete()` 刪一個**正在另一核跑**的任務（`tasks.c:2303`）。
  - `vTaskPrioritySet()` 降優先級、`vTaskSuspend()`、改 affinity、`vTaskPreemptionEnable()`。
  - `xTaskCreatePinnedToCore()` 建立的任務若即刻可搶佔另一核（`prvAddNewTaskToReadyList` 之後）。
- 訊號源本身（P4 RISC-V／S3 Xtensa）係軟件中斷，向量同 ISR 都喺 IRAM。

### 1.2 tick：**只有 core0 做 `xTaskIncrementTick()`**

- `freertos/port_systick.c:205`：SMP 下 core1 的 tick ISR 唔會 increment tick。
- 所以：**core1 上任何 `vTaskDelay()`／逾時到期，都係由 core0 的 tick ISR 幫它 unblock**，
  然後可能發 cross-core yield 給 core1。
- 呢個係「冇人 notify 都會有跨核中斷」的來源：只要 core1 有任務用 `vTaskDelay` 或帶 timeout 的等待。

### 1.3 esp_ipc（`esp_ipc_call_blocking` / `nonblocking`）

- 實作：`esp_system/esp_ipc.c`；每個核有一條 `ipc_task`，優先級 = `configMAX_PRIORITIES-1`（24）。
- `esp_ipc.c:157` 用 `xTaskNotifyGive(s_ipc_task_handle[cpu])` 叫醒對面 → 又係 cross-core yield。
- blocking 版本另外用 semaphore 等對方 ack。

### 1.4 flash 快取同步（最貴）

`spi_flash/cache_utils.c:120` `spi_flash_disable_interrupts_caches_and_other_cpu()`：

1. 取 `spi_flash_op_lock`（recursive mutex，跨核互斥）
2. 對另一個核 `esp_ipc_call_nonblocking(..., spi_flash_op_block_func, ...)`
3. 對方核喺 `spi_flash_op_block_func` 裡面：**關非 IRAM 中斷、關快取、然後自旋**等
4. 自己都關快取 → 做 erase／write → 反序恢復

⇒ 任何 flash 操作（**讀、寫、擦**）都會令另一個核完全停擺（唔止慢，係停）。
`CONFIG_SPI_FLASH_AUTO_SUSPEND` 在我們的 sdkconfig **冇開**（`/tmp/nc_sdkconfig_p4:3709`），
所以連讀都要關快取：`SPI_FLASH_CACHE_NO_DISABLE = 0`。

| 操作 | 每次凍結對方核大約 |
|---|---|
| 讀 ≤16 KB（`MAX_READ_CHUNK`） | 0.1–0.5 ms |
| 寫 4 KB | ~1.6 ms |
| 擦 4 KB sector | ~36 ms |
| 擦 64 KB block | ~262 ms |

> 呢個係「檔案系統／config／OTA 全部都會干擾解碼」的根因，唔止 OTA。

### 1.5 其他（本專案目前無影響）

| 機制 | 現況 |
|---|---|
| TWDT（`CONFIG_ESP_TASK_WDT_TIMEOUT_S=5`，兩核 idle 都檢查） | 只用 core0 timer 讀狀態；超時才 log／panic，**唔會**定期打另一核 |
| `esp_timer` | task 釘 core0；我們只用 `esp_timer_get_time()`（讀計數器），冇 callback |
| `esp_ipc_isr_*` | IDF 只用喺 sleep/panic 路徑，我們冇用 |
| GPIO ISR | 我們冇裝。`BOOT_BUTTON_GPIO` 只剩 `rtc_gpio_deinit`；`main.c` 檔頭寫的「按住 BOOT 3 秒」逃生門**目前冇實作** |

---

## 2. 我們 firmware 的實際觸發點

> 核心分配（預設）：`decode0`→core0、`decode1`→core1（有 UART/TCP 線去 core1 才建立）、
> `slow_worker`→core0、`fx`→core1、`monitor`→core0、`nc4_udp`→core1、
> `tcp_nc4`（accept）→core0、`tcp_nc4_rx`→`s_rx_core`（預設 1，只有 PUSH 才建）、
> httpd（WS）→core1、TinyUSB→core1、MSC async IO→無 affinity、lwIP tcpip→無 affinity。

### 2.1 會打斷（類 1／2／3）

| 位置 | 機制 | 幾時發生 | 預設 |
|---|---|---|---|
| [nc4_bus.c:74](../../firmware/components/sys/nc4/nc4_bus.c#L74) `nc4_bus_kick()` → `xTaskNotifyGive(decodeN)` | 1 | TCP 分核（`rx_split=1`）：RX task 收到資料就叫 decode | **唔會**（預設 PULL） |
| [nc4_bus.c:525](../../firmware/components/sys/nc4/nc4_bus.c#L525) `ulTaskNotifyTake(..., 10 ms)` | 1（經 tick） | decode 任務閒置時每 10 ms 逾時 → core0 tick 幫佢 unblock → 叫醒 core1 | **會**：decode1 存在就 100 次/秒 |
| [main.c:2255/2266](../../firmware/main/main.c#L2255) `nc4_bus_kick()` | 1 | TCP PUSH 每批資料 | 唔會（預設） |
| [nc4.c:35](../../firmware/components/sys/nc4/nc4.c#L35) decoder mutex | 4（give 時可能 → 1） | decode0／decode1／udp／WS 任兩邊同時解碼 | **會**（UART 有流量就係） |
| [nc_ota_action.c:239](../../firmware/components/action/ota_action/nc_ota_action.c#L239) `esp_ota_write()` | **3** | OTA 每 4 KB chunk（erase 時 36 ms/次） | 冇 OTA 就唔會 |
| [nc_store.c](../../firmware/components/sys/store/nc_store.c) FatFS **讀**（`nc_cfg_load`、`FILE_QUERY`、config GET、`.fxe` 載入） | **3** | 每次讀 ≤16 KB 一個 IPC | **每次讀檔**（開機、HTTP GET、列目錄） |
| [main.c:1160](../../firmware/main/main.c#L1160) `nvs_set/commit`（NET_TUNE） | **3** | `0xF00A` 寫 NVS | 有人打就係 |
| [main.c:1502](../../firmware/main/main.c#L1502)、[main.c:1525](../../firmware/main/main.c#L1525) `nvs_commit`（SET_MASTER） | **3** | 改 master CID | 有人打就係 |
| [nc4_http.c:74](../../firmware/components/sys/transport/nc4_http.c#L74) `fopen/fwrite` 寫 `system.json` | **3** | HTTP PUT config（httpd 在 **core1**） | 有人 PUT 就係 |
| [nc_store.c:303](../../firmware/components/sys/store/nc_store.c#L303) `s_lock` + FatFS 寫入 | 4 ＋ **3** | MSC 拖檔（TinyUSB core1／async IO 無 affinity）、file action（decode 核） | 拖檔就係 |
| TinyUSB MSC async IO task（`CONFIG_TINYUSB_MSC_ASYNC_IO=y`，task 無 affinity，`tinyusb_msc.c:1112`） | **3** | 每個 block 寫入 | 拖檔就係 |
| [nc4_udp.c:187](../../firmware/components/sys/transport/nc4_udp.c#L187) `vTaskDelete(s_task)` | 1 | `nc4_udp_stop()`（目前**零呼叫**） | 唔會（死碼路徑） |
| [nc_task_loop.c:132/135](../../firmware/components/sys/task/nc_task_loop.c#L132) `xTaskCreatePinnedToCore` | 1 | 開機建立 pin 去另一核的 loop（`decode1`、`fx`、`hello`…） | 開機一次性 |

### 2.2 唔打中斷，但跨核阻塞（類 4）

| 位置 | 咩事 |
|---|---|
| [nc4.c:35](../../firmware/components/sys/nc4/nc4.c#L35) | **一個 decoder 一把 mutex**：兩核的解碼被序列化。呢個係「第二核只平行解析、唔平行執行 handler」的實際來源 |
| [nc_store.c:31/303](../../firmware/components/sys/store/nc_store.c#L303) | FAT／MSC 的 `s_lock`：TinyUSB（core1）同 decode（core0/1）爭 |
| [nc4_ws.c:23](../../firmware/components/sys/transport/nc4_ws.c#L23)、[nc4_udp.c:22](../../firmware/components/sys/transport/nc4_udp.c#L22) `s_tx_lock` | 同一條 WS／UDP 的送出路徑（WS 在 core1、UDP 在 core1，多數同核） |
| [nc_mem.c:38](../../firmware/components/sys/mem/nc_mem.c#L38) `portMUX_TYPE` | 兩核同時 malloc/free 就自旋（關中斷自旋）。**目前 pool／arena API 零呼叫**，未有實際爭用 |
| [nc4_bus.c:44](../../firmware/components/sys/nc4/nc4_bus.c#L44) `portMUX_TYPE mux` | 插槽狀態機；producer／consumer 同核時無感，跨核時自旋 |
| CDC TX/RX FIFO（TinyUSB `CFG_FIFO_MUTEX=OSAL_MUTEX_REQUIRED`） | decode0（core0）同 TinyUSB task（**core1**）搶同一個 FIFO 的 OSAL mutex：每次 CDC 回覆都可能跨核交接 |
| lwIP `recvfrom`（UDP，core1） | 資料到 → tcpip task（無 affinity）喚醒 core1 的 udp_task |
| ETH ISR（core0）→ esp_eth task → lwIP | 每個 RX 中斷都在 core0；之後可能喚醒 core1 的任務 |

> 注意：**lwIP 的 tcpip task 冇 pin**（`CONFIG_LWIP_TCPIP_TASK_AFFINITY_NO_AFFINITY=y`），
> 所以「網路喚醒」係唔確定去邊個核的 —— 量測時要當佢會搬。

### 2.3 共享狀態但完全冇鎖（唔係中斷，係正確性問題）

| 位置 | 狀態 | 風險 |
|---|---|---|
| [nc_epoch.c](../../firmware/components/sys/epoch/nc_epoch.c) | `rounds/exec/cycles/...`（`nc_epoch_stats_t` 全部純量） | 每個核的 loop 寫自己 core 的格；status 由另一個核讀 → 會讀到半新半舊（數字唔準，但唔會越界）。**冇 lock、冇 barrier** |
| [nc_task_priv.h](../../firmware/components/sys/task/nc_task_priv.h) `nc_task_rt_t` | `enabled`（非 volatile）、`total_us`（64-bit） | 管理器改 `enabled` 對另一核的 trampoline 冇保證；64-bit 讀取可能撕裂 |
| [nc_hub.c](../../firmware/components/sys/mem/nc_hub.c) | `state[]` 用普通寫入做 READY／IDLE 轉換 | producer／consumer 唔同核時，冇 acquire/release 語義（依賴 cache 一致 + 時序，冇正式保證） |
| [nc4_bus.c](../../firmware/components/sys/nc4/nc4_bus.c) 插槽 | `slot_state` 在 `taskENTER_CRITICAL` 內改，資料在鎖外寫 | 有 spinlock 做 release barrier，暫時OK；但如果改成 lock-free 要補 barrier |
| [main.c](../../firmware/main/main.c) `s_rx_*` 診斷計數 | `volatile uint32_t`，多核寫 | 數字可能唔準（可接受） |

---

## 3. 預設設定下，跨核中斷有幾多

以 P4（UART→core1、TCP→core0、CDC→core0、fx→core1）為例：

| 來源 | 次數 | 備註 |
|---|---|---|
| decode1 的 10 ms 逾時 | **100 /s** | 閒置都有；有 UART 流量時變成「每幀一次 notify?」→ 其實 UART 係 PULL，冇 notify，仍然係 10 ms 一次 |
| `fx` 的幀延遲（`vTaskDelay`） | 幀率（例如 60 /s） | core1 |
| `hello`（S3 `tasks.json`，core1，1 s） | 1 /s | |
| decoder mutex 交接 | 有流量時 ≈ 每幀 | 兩核都解碼才會有 |
| OTA 4 KB | 每 chunk 1 次 IPC ＋對方核凍結（寫 1.6 ms／擦 36 ms） | 1 MB ≈ 256 次 |
| FAT 讀（開機 config、HTTP GET config、FILE_QUERY） | 每次讀檔 | 每次 ≤16 KB 一個 IPC；對方核凍結 0.1–0.5 ms |
| FAT 寫（拖檔、HTTP PUT config） | 每個 block | MSC async IO 無 affinity → 可能喺任何核 |
| NVS commit | 每次改 master／NET_TUNE | |

> 即係：**就算完全冇資料流，只要有一條 core1 任務用 `vTaskDelay`，每秒都有固定 100 次跨核中斷。**
> 呢個係 tick 機制決定，唔係我們寫錯。

---

## 4. 要留意嘅「隱形」跨核影響

1. **寫 flash = 另一個核停擺**。所以「core1 解碼、core0 寫 flash」係假平行。
   OTA 期間 decode 延遲會直接反映喺 `epoch` 的 `exec` / `period`。
2. **ISR 唔可以碰 PSRAM**：flash 操作期間對方核快取關閉，IRAM ISR 如果讀 PSRAM（hub／WS/UDP 緩衝）會爆。
3. **`vTaskDelete(remote)`** 會打斷對方核，而且刪一條「正在跑」的任務本身有 race；
   [nc4_udp.c:187](../../firmware/components/sys/transport/nc4_udp.c#L187) 呢條路徑冇人用，但留住係地雷。
4. **`prvYieldForTask` 只會叫醒「優先級最低」嗰個核**；如果對方核跑住更高優先級任務，
   中斷唔會發生，但代價係「延遲到對方核自己讓出」。
5. **兩個核都 `portENTER_CRITICAL(&global_mux)` 時，輸家關中斷自旋** —— 唔係中斷，但延遲可能 > 100 µs。

---

## 5. 點樣量（未做）

| 想知 | 做法 |
|---|---|
| 跨核中斷幾多次 | 喺 `nc4_bus_kick()` 加 counter（只覆蓋我們自己發的 notify）；系統級要用 `esp_crosscore_int_send_yield` 的 hook（IDF 冇公開 API，要 patch 或 gdb） |
| flash 凍結幾耐 | 寫 flash 前後 `esp_timer_get_time()`，同時睇另一核 `epoch.cycles` 與 `exec` 的差 |
| mutex 爭用 | 在 `nc4.c` 的 lock／unlock 加 `esp_timer` 量等鎖時間（decode 任務內） |

---

## 6. 抽出來之後可以點收斂（未定案）

1. **核歸屬單一化**：decode 全部 core0，core1 只留 fx／租戶 → 跨核只剩下 tick 與 flash，
   而且 `decode1` 根本唔會建立（`nc4_want_core1()` 回 false）→ 10 ms 逾時嗰 100 次/秒直接消失。
2. **PULL → PUSH 唔一定好**：PUSH 會多發 notify（跨核中斷），換來低延遲；要按通道決定。
3. **flash 寫入集中一核**（例如全部慢工作入 `slow_worker`@core0），另一核保持乾淨；
   代價係 OTA／FAT 都要排隊。
4. **共享狀態**：`nc_epoch`／`nc_task_rt` 可以接受「數字半新半舊」就明文寫低；
   `nc_hub` 如果要跨核正式用，要補 release/acquire（或用 FreeRTOS queue 代替）。
