# 核隔離：不變式（2026-11-04 定義）

> 一句：**兩個核 = 兩個獨立系統。唯一接口 = 緩衝。**
> 任何一個核都唔應該因為另一個核而收到中斷、被阻塞、或者被改變。
>
> 呢份係**要求**；現況證據同逐條位置喺 [cross-core.md](cross-core.md)。

---

## 1. 不變式

| | 不變式 | 意思 |
|---|---|---|
| **I1** | 中斷隔離 | B 核永遠唔會因為 A 核的行為而收到任何中斷：冇 cross-core yield、冇 IPC、冇 flash 快取同步 |
| **I2** | 狀態隔離 | 兩核之間冇共享**可變**狀態；唯一共享係緩衝（slot 區），而且協議只有「寫 → 標記 ready → 對方讀」，冇 signal／mutex／queue |
| **I3** | 時間隔離 | 每個核只由自己的時間源推進（自己的迴圈、自己核的中斷）；冇任何「等另一個核」的等待 |

> I3 係最容易被忽略嗰條：就算冇 IPC，只要 core1 的任務用 `vTaskDelay`，
> 它的逾時都係由 **core0 的 tick ISR** 處理，然後 core0 發跨核中斷叫醒它。

---

## 2. 由不變式直接推出的硬性條件（逐條可驗證）

1. **核上唔可以有會逾時的等待**
   `vTaskDelay`、`xSemaphoreTake(t>0)`、`ulTaskNotifyTake(t>0)`、`select/recv` 帶 timeout、
   `esp_timer` callback、任何 `period_ms` 迴圈 —— 全部唔可以出現在非 core0 的核。
   → 只剩 **non-blocking poll**（`uart_read_bytes(...,0)`、`recv(MSG_DONTWAIT)`、讀 slot 狀態）。
2. **核上唔可以有跨核同步原語**：queue／semaphore／notify／event group 一律只可以核內用。
3. **唔可以操作另一個核的任務**：`vTaskDelete(remote)`、改對方 priority／affinity、
   `vTaskSuspend(remote)` 全部禁止。
4. **decoder 唔可以共用一把 mutex**：每個核自己一份 decoder（handler 要 reentrant），
   或者所有 handler 釘死同一個核。
5. **flash 只可以有一個擁有者**：讀、寫、擦都會 IPC 去另一個核（除非開 Auto-Suspend）。
   另一個核要用 flash 就交 job 落緩衝，由擁有者代做。
6. **所有 IDF 任務要明文 pin**（lwIP、TinyUSB、esp_eth、esp_timer…），
   否則佢會喺兩個核之間游走，I3 即刻破。
7. **同一核唔可以有兩條同優先級的 ready 任務**（`configUSE_TIME_SLICING=1`：
   core0 每 1 ms 會為 core1 的時間片發跨核中斷）。
8. **TWDT 唔可以檢查另一個核的 idle**（`CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1`）：
   純迴圈唔會讓出 CPU，idle 永遠唔跑 → 5 秒誤報。
9. **共享緩衝要用內部 SRAM**：內部 SRAM 唔經快取，兩核天然一致；
   PSRAM 有快取，跨核交換要另外處理（msync／只做單向 bulk）。
10. **狀態／量測要經緩衝**：`epoch`、任務統計、RX 計數等等，唔可以直接讀另一個核的變數
    （讀到半新半舊事小，破壞 I2 事大）。

---

## 3. 現況：違反清單

| # | 違反 | I1／I2／I3 | 位置 | 現況 |
|---|---|---|---|---|
| V1 | core1 的 loop 用 `vTaskDelay`（trampoline 的 `period_ms`） | I1 | [nc_task_loop.c:98](../../firmware/components/sys/task/nc_task_loop.c#L98) | 任何 core1 任務有 period 就違反 |
| V2 | decode1 用 10 ms 逾時等資料 | I1 | [nc4_bus.c:525](../../firmware/components/sys/nc4/nc4_bus.c#L525) | 閒置都 **100 次/秒**（經 core0 tick） |
| V3 | 一個 decoder 一把 mutex（跨核） | I1＋I2 | [nc4.c:35](../../firmware/components/sys/nc4/nc4.c#L35) | 兩核解碼序列化；give 會叫醒另一核 |
| V4 | flash 讀／寫／擦全部 IPC ＋凍結對方核 | I1 | OTA／NVS／FAT／config 讀取 | `SPI_FLASH_AUTO_SUSPEND` 未開；0.1–262 ms／次 |
| V5 | CDC FIFO 的 OSAL mutex | I1＋I2 | decode0（core0）↔ TinyUSB（core1） | 每次 CDC 回覆 |
| V6 | `nc4_bus_kick()` 跨核 notify | I1 | [nc4_bus.c:74](../../firmware/components/sys/nc4/nc4_bus.c#L74) | 預設關（PULL），開 PUSH 就中 |
| V7 | UDP `recvfrom` 阻塞喺 core1 | I1 | [nc4_udp.c:169](../../firmware/components/sys/transport/nc4_udp.c#L169) | 每個 datagram 由 lwIP 喚醒 |
| V8 | `vTaskDelete(remote)` | I1 | [nc4_udp.c:187](../../firmware/components/sys/transport/nc4_udp.c#L187) | 零呼叫（死路徑），要刪 |
| V9 | lwIP tcpip task 冇 pin | I3 | `CONFIG_LWIP_TCPIP_TASK_AFFINITY_NO_AFFINITY=y` | 網路喚醒會飄去另一核 |
| V10 | TWDT 檢查 CPU1 idle | I1（間接） | `CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1=y` | 純迴圈 core1 會 5 秒誤報 |
| V11 | 共享可變狀態無鎖／無 barrier | I2 | `nc_hub`、`nc_epoch`、`nc_task_priv`、`s_rx_*` | status 讀到半新半舊 |
| V12 | 開機由 core0 建立 pin 去 core1 的任務 | I2（一次性） | [nc_task_loop.c:132](../../firmware/components/sys/task/nc_task_loop.c#L132) | 初始化例外，要明文寫落文件 |

---

## 4. 三個要拍板的結構問題

### D1. core1 仲係咪 FreeRTOS 任務？

| 選擇 | 好處 | 代價 |
|---|---|---|
| **A. 係，但「永不阻塞」**（單一 loop，唔用任何會 timeout 的 API） | 改動中等；仍然有 stack／可 debug | 要自己輪流跑多條 loop（即 group）；idle 永遠唔跑，WDT 要關 |
| **B. 離開 scheduler（bare-metal）** | 真正零 scheduler 互動 | 大改；失去 IDF 任務模型、driver 安裝方式要重做 |

> 建議 **A**：FreeRTOS 只係「借個 stack 同 context」，core1 之後唔再呼叫任何會睡的 API。
> 呢個就係 [execution-model.md](execution-model.md) 的 `group`（局部 B）。

### D2. flash 由邊個核擁有？

| 選擇 | 結果 |
|---|---|
| **A. 全釘 core0 ＋ 開 `CONFIG_SPI_FLASH_AUTO_SUSPEND`** | 冇 IPC、冇凍結；代價：靠 flash chip 支援 suspend，erase/write 有額外開銷，要上板驗 |
| B. 全釘 core0，唔開 Auto-Suspend | core0 每次 flash op 都會 IPC 去 core1 → 直接違反 I1（唔可以接受） |
| C. 兩核都可以碰 flash | 更差 |

> 即係：**I1 要成立，Auto-Suspend 基本上係必要條件**（兩個 target 的 SoC 都報 `SOC_SPI_MEM_SUPPORT_AUTO_SUSPEND=y`，
> 但要確認實際 flash 型號支援）。

### D3. 量測／狀態數字算唔算「共享狀態」？

- 嚴格：`epoch`、任務統計都要每個核自己 snapshot 落 buffer，再由 core0 讀 buffer。
- 寬鬆：允許「唯讀、單向、容許撕裂」的數字（明文寫低係 best-effort）。

建議先行**寬鬆 + 明文標記**，因為呢啲數字唔影響功能；但 `STATUS_GET` 唔可以因此去叫醒另一個核。

---

## 5. 邊界（做唔到完美的地方，要老實寫）

1. **核心共享硬體**：兩核共用記憶體匯流排／快取控制器，物理上冇得隔離。
   「隔離」講的是**軟件可見行為**：冇中斷、冇阻塞、冇共享可變狀態。
2. **FreeRTOS kernel 本身**：ready list 由兩核的 context switch 共用一把 spinlock。
   只要 core1 唔呼叫任何 FreeRTOS API（唔 yield、唔 take、唔 delay），佢就唔會碰呢把鎖。
3. **開機初始化**：建立任務、pin IRQ、配置記憶體，必然由一個核做 —— 呢段係例外，要明文界定
   「scheduler 起動前／第一次 loop 之前」。
4. **ISR 仍然會發生**：每個核有自己的 timer／週邊中斷，呢啲係**本地**中斷，唔受 I1 限制。

---

## 6. 驗收（點證明做到）

| 檢查 | 方法 | 合格線 |
|---|---|---|
| core1 冇逾時 | 代碼審查 + grep：core1 路徑上冇 `vTaskDelay`／timeout／queue／mutex | 0 命中 |
| 冇跨核中斷 | 喺 `nc4_bus_kick` 加 counter；core0 做 OTA 時對比 core1 的 `epoch.period` | 抖動 < 5% |
| 緩衝協議 | hub slot 狀態用 atomic（release/acquire）；跑 hosttest 的 hub／torn 測試 | torn = 0 |
| flash | 開 Auto-Suspend 後，OTA 期間 core1 的 round 唔停 | 連續 3 次 OTA |
| 核心分配 | `STATUS_GET` / `epoch` 顯示任務核心同設計一致 | 100% |

---

## 7. 任務總表（現況 2026-11-04）

> 計數：P4 預設 ≈15 條、S3 預設 ≈13 條（未計測試任務）。逐條對住 §1 的三條不變式。

### 7.1 我們自己建立的

| 任務 | 核 | 邊個建立 | 等待方式 | 影響 | 處置 |
|---|---|---|---|---|---|
| `main` | 0 | IDF | 開機跑完就結束 | — | ✅ |
| `decode0` | 0 | 任務管理器 | `ulTaskNotifyTake(10 ms)` | 逾時喺 core0 本地處理 → I1 冇事；**同 core1 共用 decoder mutex**（I2） | 🔄 per-core decoder |
| `decode1` | 1 | 任務管理器 | 同上 | 逾時由 **core0 的 tick** 處理 → **I1 違反，100 次/秒** | 🔄 改 poll；或者索性唔開 |
| ~~`slow_worker`~~ | — | — | — | **已刪（2026-11-04）**：指令一問一答，慢工作佇列唔需要 | 🗑️ |
| `fx`（**POC／原始形態**） | 1 | 任務管理器 | `vTaskDelay(幀期)` [nc_fx.c:778](../../firmware/components/tasks/test_fx_tasks/nc_fx.c#L778) | **I1 違反**（每幀一次）；運作方式未定義 | 🔄 重新規劃（核心、幀來源、buffer 接口） |
| ~~`monitor`~~ | — | — | — | **已刪（2026-11-04）**：只係一行 log，`epoch`／`STATUS_GET` 已可即時查 | 🗑️ |
| `hello` | 0（S3 `tasks.json`） | 任務管理器 | `vTaskDelay(1000)` | 本地 | ✅ |
| `bench_a/b/c` | 1（預設**關**） | 任務管理器 | `vTaskDelay(5)` | 開啟時 **I1 違反，200 次/秒** | ✅ 要量先開 |
| `probe` | — | — | 只有 `on_start`，冇 loop | 唔係任務 | ✅ |
| `nc4_udp` | 1 | 裸 `xTaskCreatePinnedToCore` [nc4_udp.c:169](../../firmware/components/sys/transport/nc4_udp.c#L169) | **blocking `recvfrom`** | **I1 違反**（每個 datagram 由 lwIP 喚醒）＋共用 decoder mutex | 🔄 改 non-blocking poll，或搬 core0 |
| `tcp_nc4`（accept） | 0 | 裸建立 [main.c:2631](../../firmware/main/main.c#L2631) | blocking `accept()` | 本地；但喚醒來自 lwIP（無 pin，I3 風險） | 🔄 收編入管理器 |
| `tcp_nc4_rx` | 1（`s_rx_core` 預設 1，**只有 PUSH 才建**） | 裸建立 [main.c:2837](../../firmware/main/main.c#L2837) | `recv(DONTWAIT)`＋`vTaskDelay(1)`＋`nc4_bus_kick` | **I1 違反**（跨核 notify＋1 ms 逾時） | 🔄 或直接唔用 PUSH |
| `tcp_sink0/3`、`tcp_src0/1` | 0／1 | 裸建立（`CONFIG_NETCORE_TEST_TASKS`，預設關） | `vTaskDelay`＋blocking recv | 測試專用 → I1 | ✅ 預設關 |
| `hub_w`／`hub_r`／`tx_pack` | 1／0／1 | 測試 handler 內臨時建立 | 自旋／延遲 | 測試專用 → I1 | ✅ 只喺量測時 |
| `usb_reconn` | **無 affinity** | 裸建立 [nc_store.c:219](../../firmware/components/sys/store/nc_store.c#L219) | `vTaskDelay(800/300)` | **I1 違反**（可能落 core1 就由 core0 tick 喚醒）＋**I2**（同 TinyUSB task 爭 USB 狀態） | 🔄 pin core1；或收編 |

### 7.2 IDF 建立的

| 任務 | 核 | 影響 |
|---|---|---|
| `IDLE0` / `IDLE1` | 0／1 | 基準。core1 若永遠唔讓出，idle 唔跑 → TWDT 必須關 CPU1 檢查 |
| `esp_timer` | 0 | ✅（我們冇用 `esp_timer_create`） |
| `ipc0` / `ipc1` | 0／1 | 只被 flash／`esp_intr_free` 叫醒 → **I1 經 flash** |
| `tcpip`（lwIP） | **無 pin** | **I3 違反**：會游走；佢負責喚醒我們的 socket 任務 |
| `sys_evt`（esp_event） | 0 | ✅ |
| `TinyUSB` | **1** | **I1＋I2 違反**：CDC 資料同 **log** 都經它（§7.3） |
| `msc_async_io` | **無 pin** | **I1＋I3 違反**：拖檔時每個 block 寫 flash |
| `httpd`（WS／HTTP） | 1（`cfg.core_id=1`） | **I1＋I2 違反**：共用 decoder mutex、`fput` 寫 FAT（config PUT） |
| `wifi`（S3，`CONFIG_NETCORE_WIFI=y` 才有） | 0 | ✅ 任務本身；但 radio ISR＋事件會經 `sys_evt` |

### 7.3 冇 log 任務，但有 log **路徑**（重點）

**現況冇任何 log task**；`ESP_LOGx` 係**同步**喺呼叫者個核上面做的。問題係輸出目的地：

| 目標 | 輸出路徑 | 幾時跨核 |
|---|---|---|
| **S3** | `ESP_LOG` → stdout → **TinyUSB CDC VFS**（[main.c:351](../../firmware/main/main.c#L351) `tinyusb_console_init`）→ `tusb_write()`（`_lock_t`）→ TinyUSB FIFO（OSAL mutex）→ **core1 的 TinyUSB task** 送出 | **每一行**：跨核 mutex；而且 core0 寫 FIFO 會喚醒／爭 core1 的 USB task |
| **P4** | `ESP_LOG` → stdout → **UART0**（USB-Serial/JTAG）→ UART VFS ＋ driver ring，TX ISR 喺安裝核（開機 core0） | 由 core1 log 會爭同一條 ring／driver 鎖 |

⇒ 按 I1／I2，**非 core0 的核唔可以有任何 `ESP_LOG`**（包括 `hello_loop` 每秒印 core id、`fx`、所有錯誤路徑）。
要保留 core1 的 log，就要自己開一個 **lock-free ring**（core1 只寫、core0 只讀）再由 core0 輸出。

> 呢個就係「還有一個 log」：唔係任務，係一條**每行 log 都會踩**的跨核路徑；
> 喺 S3 上佢仲同時係 CDC 資料通道（TinyUSB core1），所以 log 同通訊會互相影響。

### 7.4 其他共享路徑（唔係任務）

| 路徑 | 性質 | 影響 |
|---|---|---|
| flash（讀／寫／擦） | IPC ＋凍結對方核 | **I1 違反**（除非開 Auto-Suspend） |
| heap（`malloc`／`free`） | 兩核共用 spinlock | I2；建議 core1 起動後唔再 alloc |
| FAT／VFS `_lock_t` | 共用 mutex | I2（任何核讀寫檔案） |
| `esp_timer_get_time()` | 唯讀共用計數器 | 可接受（無中斷、無鎖） |
| `xTaskGetTickCount()` | 讀 core0 寫的 tick | 唯讀，可接受（我們冇用） |
| `nc_hub` 緩衝 | **唯一允許**的跨核接口 | 要補 release／acquire |

---

## 8. 任務分層（2026-11-04 拍板）

| 層 | 任務 | 核心歸屬 | 等待策略 | 開關 |
|---|---|---|---|---|
| **L0 通訊**（暫時收起） | `nc4_udp`、WS／`httpd`、`tcp_nc4`、`tcp_nc4_rx` | **同 code 解綁**：唔准再喺 `main.c` 寫死核心，由 `links[].core` 決定 | 通道自己決定（PULL = 該核自己 poll） | `links[].enable` |
| **L1 解碼** | `decode0`／`decode1` | router（`nc4.routes[].out` 的 `decodeN`） | **純 poll，冇 timeout** | router |
| **L2 系統** | `monitor` | 可配置（預設 0） | 本地 | 常開 |
| **L3 功能** | `fx` | 可配置（預設 1） | 本地 | manifest |
| **L4 log**（新） | `log` | **可配置** | 有資料先印；經中間緩衝收集兩核 | 常開 |
| **L5 測試** | `hello`、`bench_a/b/c`、`probe`、`hub_w/r`、`tx_pack`、`tcp_sink/src`、`0xF00x` handlers | 測試專用 | — | **`NC_ENABLE_TESTS`（預設 OFF）** |

### 8.1 五條決議

1. **通訊層與解碼一樣要獨立分離**，而且「核心」係配置，唔係 code。呢一步**暫時收起**（唔改功能），
   但 `main.c` 之後唔可以再有 `.core = 0/1` 寫死喺通訊任務上。
2. **`slow_worker` 不存在** —— 已刪（見 §7.1；理由：指令一問一答，慢指令擋唔到下一條）。
3. **測試全部搬去 `test_*`**：`firmware/components/tasks/test_*_tasks/` ＋ `components/action/test_action/`，
   由**一個**編譯開關 `NC_ENABLE_TESTS`（預設 OFF）控制；閂咗 = 唔編譯、唔註冊、唔佔 stack。
4. **log 兩核都唔准直接印**：`ESP_LOG*` 一律入中間緩衝；由**一條** log task 印，
   「有先印、冇就唔印」；log task 自己嘅核心可以調配。
5. **每個任務都要有單一核心出處**：唔可以再有「router 講一次、NVS 講一次、code 又寫死一次」。

> **分類更正（2026-11-04 拍板）**：`fx` 係 **POC 原始形態**——燈效引擎當時用來驗證架構，
> **運作方式未定義**，唔係正式功能。`monitor` 亦只係實驗形態（只記錄、未定案）。
> 兩者都唔歸入「測試開關」（唔應該跟 release 一起消失），但**要打上 `prototype` 標記**，
> 等將來重新規劃：`fx` 要決定「邊個核 render、幀從邊度嚟、同 buffer 交換點對接」。

### 8.2 分層之後，每條任務的等待策略

| 任務 | 現在 | 目標 |
|---|---|---|
| `decode1` | `ulTaskNotifyTake(10 ms)` | poll（`taskYIELD()` 或直接再掃一次；唔可以 timeout） |
| `fx` | `vTaskDelay(幀期)` | 若留喺 core1 → 改成「用 `esp_timer` 讀時間 + 自旋到下一幀」；或者搬 core0 |
| `monitor` | `vTaskDelay(1000)` | 留 core0（本地），或按配置 |
| `nc4_udp` | blocking `recvfrom` | 收起；之後用 non-blocking + poll |
| `tcp_nc4_rx` | `vTaskDelay(1)` + kick | 收起（PUSH 模式本身違反 I1） |
| `log` | 未有 | 有資料才印；core0 可以用 semaphore，core1 只可以 poll |
| 測試 | 各式 | 唔喺正式映像，唔受不變式約束（但都要 pin） |

### 8.3 分批（未開始）

| 批次 | 內容 | 風險 |
|---|---|---|
| **B1** ✅ 已完成 | 刪 `slow_worker`＋`jobs`＋`0xF005`／`0xF006`；UART 改開機安裝 | 低 |
| **B2** | 測試搬去 `test_*` ＋ `NC_ENABLE_TESTS`（預設 OFF） | 中（CMake／目錄搬遷） |
| **B3** | 解碼：per-core decoder ＋ 非 core0 純 poll；拿掉 NVS `rx_core`／`rx_split` | 高（handler reentrancy） |
| **B4** | `nc_log`：雙核緩衝 ＋ 獨立 log task（核心可配置） | 中（S3 上 console 係 USB CDC＝core1，要對齊） |
| **B5** | 通訊層解綁：`links[].core`，`main.c` 唔再寫死 | 中 |

---

## 9. 未解耦清單（2026-11-04 盤點）

> 「解耦」= 冇共享可變狀態、冇跨核中斷、每層自己決定核心、硬體有單一擁有者。
> 下面每一條都仲未做到。

### 9.1 跨核（直接違反 I1／I2）

| # | 未解耦 | 位置 | 現況 |
|---|---|---|---|
| U1 | 一個 decoder 一把 mutex | [nc4.c:35](../../firmware/components/sys/nc4/nc4.c#L35) | decode0／decode1／udp／httpd 輪流等鎖 |
| U2 | `decode1` 的 10 ms 逾時 | [nc4_bus.c:525](../../firmware/components/sys/nc4/nc4_bus.c#L525) | 逾時由 core0 tick 處理 → 跨核中斷 |
| U3 | flash 讀／寫／擦 | `spi_flash` 路徑 | IPC ＋對方核凍結；`AUTO_SUSPEND` 未開 |
| U4 | CDC FIFO | decode0（core0）↔ TinyUSB（core1） | OSAL mutex，每次回覆 |
| U5 | log | `ESP_LOG` → UART0 或 USB CDC | 冇中間緩衝、冇 log task |
| U6 | lwIP `tcpip` 冇 pin | sdkconfig `NO_AFFINITY=y` | 會游走兩個核 |
| U7 | `msc_async_io` 冇 pin | esp_tinyusb | 拖檔時喺任何核寫 flash |
| U8 | `usb_reconn` 冇 pin | [nc_store.c:219](../../firmware/components/sys/store/nc_store.c#L219) | 可能落 core1；動 USB 狀態 |
| U9 | `nc4_udp` 阻塞 `recvfrom` | [nc4_udp.c:169](../../firmware/components/sys/transport/nc4_udp.c#L169) | 每個 datagram 由 lwIP 喚醒 |
| U10 | TCP PUSH 的 `nc4_bus_kick` | [main.c:2255](../../firmware/main/main.c#L2255) | 跨核 notify（預設關） |
| U11 | `vTaskDelete(remote)` | [nc4_udp.c:187](../../firmware/components/sys/transport/nc4_udp.c#L187) | 死路徑，未刪 |

### 9.2 層與層（解耦之後才有「獨立分離」）

| # | 未解耦 | 現況 |
|---|---|---|
| L1 | 通訊層住在 `main.c` | accept／rx／ETH glue／UDP／WS 全部喺 `app_main`，核心寫死 `.core=0/1` |
| L2 | 解碼核心的決定散落 | router 決定 + `nc4_want_coreN()` + `decodeN` loop 寫死 core |
| L3 | 測試散在 3 個地方 | `app/tasks/bench`、`app/tasks/hello`、`main.c`（probe／hub／tx／tcp sink·src／0xF00x） |
| L4 | 開關有 2 套 | `CONFIG_NETCORE_TEST_TASKS`（Kconfig）+ `#if`；文件寫的係 `NC_ENABLE_TESTS`（CMake） |
| L5 | 指令表產品＋測試同一張 | `s_core_actions[]` 13 條，8 條係測試 |
| L6 | log 冇層 | 冇 component、冇緩衝、冇任務 |
| L7 | 核心歸屬 4 個來源 | router／NVS `net/rx_core`／code 寫死（fx、udp、accept、TinyUSB）／`tasks.json` |
| L8 | 任務建立 2 條路 | 任務管理器 vs 裸 `xTaskCreatePinnedToCore`（4 處） |
| L9 | 記憶體 2 套 | `nc_plan` 宣告 vs 執行期 `malloc`（WS/UDP rx、decoder、hub registry、reply） |

### 9.3 硬體擁有權（每個硬體要一個「主人」）

| # | 硬體 | 現況 |
|---|---|---|
| H1 | flash | 冇單一主人：decode 任務（OTA／NVS／FAT）、httpd（core1）、MSC 任務（無 pin）都寫得 |
| H2 | UART1 | ISR 喺開機核（core0），consumer（decode1）可能喺 core1 |
| H3 | USB CDC | 由 core1 的 TinyUSB 擁有，但 core0 直接讀寫 FIFO |
| H4 | ETH（P4） | ISR 喺 core0；lwIP 無 pin；我們的 TCP 任務散喺兩核 |
| H5 | WiFi（S3） | task 喺 core0；radio ISR 唔喺我們控制內 |
| H6 | console（log 出口） | P4 = UART0（core0）；S3 = USB CDC（core1）—— 同 log task 的核心要對齊 |

### 9.4 尚未定義（未設計，唔係欠債）

| # | 題目 | 為咩未解耦 |
|---|---|---|
| D1 | `fx` 運作方式 | POC 原始形態：render 喺邊個核、幀從邊度嚟、同 buffer 交換點接口都未有 |
| D2 | log task 的核心 | 要等「console 擁有者」定案（S3 係 core1） |
| D3 | 緩衝協議 | hub slot 要 release/acquire；用內部 SRAM 定 PSRAM 未定 |
| D4 | 通訊層核心解綁後 | driver ISR 同 consumer 可以唔同核？定係「一條 link 一個核全包」 |
| D5 | per-core decoder | handler 可重入規則（而家 handler 有 static 嗎？） |
| D6 | TWDT 替代 | core1 純迴圈後 idle 永遠唔跑，要靠本地看門狗（任務自己數 cycle） |
| D7 | flash 單核 + AUTO_SUSPEND | 要上板驗 flash 型號支援同速度代價 |

> 一句：**9.1 係硬違反，9.2 係結構，9.3 係擁有權，9.4 係未設計。**
> 三個「大石」：flash（U3／H1／D7）、log（U5／H6／D2）、decoder（U1／D5）。

---

## 10. 現況（2026-11-05 00:10）：仍然未分開的

> 測試層已經清乾淨（`test_action`、`test_*_tasks` 全部唔喺正式映像），
> `slow_worker`／`monitor` 已刪。下面淨返嘅係**真功能**層面的跨核問題。

### 10.1 正式映像（tests OFF）仍然存在

| # | 未分開 | 位置 | 影響 |
|---|---|---|---|
| R1 | ~~`decode1` 用 `ulTaskNotifyTake(10 ms)`~~ **已更正（2026-11-05 驗證）**：只有「該核有 PULL bus」才用 10 ms，其餘 `portMAX_DELAY` | [nc4_bus.c](../../firmware/components/sys/nc4/nc4_bus.c) `nc4_bus_task_entry` | 預設 core1 只有 UART（PUSH）⇒ **無限等、零逾時喚醒**；10 ms 輪詢只剩 core0（CDC＋TCP 都係 PULL）。而且 tick 係每核自己的 systimer，**唔會跨核**，所以原本「100 次/秒跨核中斷」的說法不成立 |
| R2 | 一個 decoder 一把 mutex | [nc4.c](../../firmware/components/sys/nc4/nc4.c) | decode0／decode1／httpd（P4）／udp（P4）輪流等鎖 |
| R3 | flash 讀／寫／擦 | `spi_flash` 路徑 | IPC ＋凍結對方核（`AUTO_SUSPEND` 仍未開）；FAT／NVS／OTA／config 全部中 |
| R4 | CDC FIFO | decode0（core0）↔ TinyUSB（core1，S3） | 每次 CDC 回覆跨核 mutex |
| R5 | log | 冇 `nc_log`，每行直接落 console | S3 經 TinyUSB（core1）、P4 經 UART0（core0） |
| R6 | lwIP `tcpip` 冇 pin | `CONFIG_LWIP_TCPIP_TASK_AFFINITY_NO_AFFINITY=y` | 會游走兩核（P4） |
| R7 | `msc_async_io` 冇 pin | esp_tinyusb | 拖檔時任何核寫 flash（S3） |
| R8 | `usb_reconn` 冇 pin | [nc_store.c](../../firmware/components/sys/store/nc_store.c) | 可能落 core1（S3，切換 file_mode 時） |
| R9 | UDP `recvfrom` 阻塞（core1） | [nc4_udp.c](../../firmware/components/sys/transport/nc4_udp.c) | 每個 datagram 由 lwIP 喚醒（P4） |
| R10 | heap 共用 | `malloc`/`free` | 兩核共用 spinlock（decode 路徑有分配就爭） |
| R11 | 量測共享 | `nc_epoch`／`nc_task_rt`／hub slot | 跨核讀，無 barrier（數字唔準，但唔影響功能） |

### 10.2 已經分開（今輪完成）

| 項目 | 之前 | 之後 |
|---|---|---|
| `slow_worker` | core0 常駐 worker + jobs hub | 已刪 |
| `monitor` | core0 每秒讀兩核 epoch | 已刪 |
| `fx`（POC） | core1，每幀 `vTaskDelay` | 只喺 `NC_ENABLE_TESTS=ON` 存在 |
| `probe`／`tcp_sink/src`／`bench_*`／`hello` | 散在 `main.c`／正式映像 | `tasks/test_*_tasks`，OFF 時唔存在 |
| 8 條測試指令 | `main.c`，永遠編入 | `commands/test_action`，OFF 時唔存在 |
| 正式指令表 | 13 條（8 條測試） | **5 條** |

### 10.3 一個 config 就解決得掉的部分

`decode1` 之所以存在，只因為 router 有線去 core1（預設 UART→core1）。
把 **UART 也指去 core0**（`system.json` 的 `routes[].out: ["decode0"]`），
`decode1` 就唔會建立：R1 直接歸零、R2 只剩 decode0 與 httpd／udp。

代價：UART 同 CDC／TCP 共用 core0 的解碼時間；核心1 只剩 IDF 任務。

### 10.4 剩下三個「大石」的次序

1. **R3 flash**（要先驗 `CONFIG_SPI_FLASH_AUTO_SUSPEND`）——決定 I1 成唔成立
2. **R1／R2 解碼**（per-core decoder ＋ 非 core0 純 poll）
3. **R5 log**（緩衝 ＋ 獨立任務，核心跟 console）

---

## 11. CDC receiver：核隔離實測（2026-11-05）

`cdc.rx`（receiver task）同 `decode0` **都釘喺 core 0**；`kick` 由 receiver 發出，
同核 notify，唔會經 IPC。以下係上板實測（包含一次完整 OTA：4 KB × 96 包）：

| 指標 | 值 | 判讀 |
|---|---|---|
| `remote`（跨核 kick 次數） | **0** | 冇任何跨核喚醒 ← 核心隔離的硬指標 |
| `crc_err` / `bad_hdr` | 0 / 0 | 冇干擾造成的錯幀 |
| `peak`（環峰值） | 2 / 4 格 | 環夠用；`avg_b` = 65 B／次 |
| `hwm`（receiver stack 剩餘） | 696 / 1,536 | 實測最多用 840 B |
| RTT p50 / p95 / p99 / max | 0.59 / 0.65 / **0.75** / 0.77 ms | 分佈極窄，冇抖動 |

**配置**：`region 4,192`、環 4 × 256 B、stack 1,536、prio 5、core 0。
相對 PULL（10 ms、9,229 B）＝ **記憶體幾乎一樣（＋836 B），延遲快 19 倍**。

> **注意**：`cdc.rx` 的 prio 5 同 **TinyUSB task 的 prio 5 同核** ⇒ 同 prio 會 1 ms tick
> 輪轉。實測 RTT p99 = 0.75 ms 已經包含呢個成本；如果要再收窄，就要調開其中一邊的 prio。

**仍然存在的跨核耦合**（同 CDC receiver 無關，屬第 10 節的 R 系列）：
R3 flash、R4 console/CDC FIFO、R5 log、R6 lwIP 未釘核、R8 `usb_reconn` 未釘核、R10 heap。
