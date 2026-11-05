# 執行規則：調度、搶佔、等待、核心隔離

> **用途**：把「任務管理器／任務框架」與 FreeRTOS 的界線、搶佔條件、等待規則、核心隔離規則
> 集中成一份**可引用、可檢查**的規則。這是 `execution-model.md` 的上位文件：
> 那份講「A 還是 B」，這份講「無論 A 還是 B，邊條線唔可以踩」。
>
> **對象**：寫 loop 的人（含未來的自己）。
> **狀態**：規則已定案；第 8、9 節的裁決待執行。

---

## 0. 定位：管理器是 FreeRTOS 的輔助層

> **Manager 唔可以創造，亦唔可以取消一個 scheduling point。**
> 佢只可以在「已經拿到 CPU 之後」決定次序、記數字、判預算、決定停唔停。

| 問題 | 邊個答 |
|---|---|
| 幾時行 | FreeRTOS（tick／block／notify） |
| 邊個行先 | FreeRTOS（prio）；**只有 group 之內**才輪到管理器 |
| 邊個 core | FreeRTOS（pinned）；管理器只負責**寫入** pin 值 |
| 會唔會被打斷 | FreeRTOS（中斷＋搶佔）；管理器只能**事後記錄** |
| 一輪有邊幾步、乜嘢次序 | 任務框架 |
| 每步幾久、幾多次、有冇超標 | 管理器 |
| 邊個 enabled、幾時停、幾時重啟 | 管理器 |
| 堆疊／緩衝要幾多 | **任務自己宣告**，管理器只匯總、分配、驗證 |

**「調度」一詞要拆開**：

- **配置**（管理器做）：開機時寫入 `prio` 同 `core` 兩個數字。
- **調度**（FreeRTOS 做）：每一刻決定邊條 task 行。

管理器可以**設定**參數，唔可以**執行**調度。兩者唔可以混淆。

**現況**：每一條 loop 都係一條真 FreeRTOS task（`xTaskCreatePinnedToCore`，見
`nc_task_loop.c`）。冇自寫 scheduler、冇自寫 tick、冇改 context switch。
管理器 = 一張宣告表 + 一條 `trampoline` wrapper。

---

## 1. 搶佔的條件

> **搶佔唔係「計時」發生，係「事件」發生。**

```
搶佔 = R.prio > T.prio
     ∧ R 可以行喺 T 嘅 core（pinned 到該核，或 unpinned）
     ∧ R 由 blocked 變成 ready        ← 重點：唔係「存在」
     ∧ 當刻有 scheduling point（ISR 出口／API 返回／tick）
     ∧ T 冇屏蔽（critical section／flash 操作／關中斷）
```

| 條件 | 說明 |
|---|---|
| 更高 prio | 同 prio **唔會**即時搶（只會被 tick 切片）；低 prio **永遠**搶唔到 |
| 同一個 core | pinned 只在自己核競爭；**unpinned 兩核都可以，所以會飄** |
| blocked → ready | 已經 ready 嘅高 prio，調度器一開始就揀佢，你唔會行 |
| scheduling point | 跑純計算中間冇 point |
| 冇屏蔽 | `taskENTER_CRITICAL`、關中斷、SPI flash 寫入（scheduler 停，**兩核一齊停**） |

**同 prio 切片的三個條件**：`configUSE_TIME_SLICING = 1`（IDF 預設，冇 Kconfig 可關）、
該核仲有另一條同 prio ready task、切點在 tick（相位任意，1 tick = 1 ms）。

**FreeRTOS 保證的只有一件事**：任何時刻 running 嘅 task，係該核所有 ready task 中 prio 最高者
（同 prio 則輪流）。**就係咁多。**

### 1.1 實際事件源

| 源 | 幾時 | 成本 |
|---|---|---|
| Tick 1 kHz（systimer） | 每 1 ms | 1–3 µs；只處理 delay 到期／同 prio 切片 |
| EMAC ISR（P4） | 收到封包 | 中斷幾 µs，**叫醒嘅 task 才係大數** |
| USB OTG ISR（S3 TinyUSB） | 主機讀寫 | 同上 |
| USB-Serial-JTAG | console 有讀寫 | 叫醒 console 處理 |
| IPC ISR | 另一核 notify 你 | **本專案禁止**（見第 2 節） |
| esp_timer ISR | 有 timer 到期 | 只喺 **core 0**；冇 timer 就唔 fire |

**冇** SPI／I2C／RMT／GPIO／ADC／SD／BT 中斷（全 firmware 冇 `esp_intr_alloc`）。

### 1.2 esp_timer 是甚麼

- 硬件：**systimer** 的 alarm（同 FreeRTOS tick 同一組硬體、**唔同 alarm**）。
  唔係 S3 那 4 個 64-bit 通用定時器（嗰啲係 `gptimer`）。
- 軟件：一個 ISR ＋ 一條 task（prio 22、**core 0**、stack 3584 B）。
- 開機由 `esp_timer_early_init()` 建立；**冇 timer 就唔會 fire**。
- `esp_timer_get_time()` 只係讀 counter，**唔會產生中斷**。
- 本 firmware **冇 create 任何 esp_timer**；實際會 fire 的只有 TWDT（5 s，檢查兩個 idle task）。
- **對性能影響：零**（固定成本 = 1 條 task 的 3.5 KB stack）。而且永遠唔會去 core 1。

---

## 2. 核心隔離：跨核只准有「資料」，唔准有「喚醒」

**設計目標：一個核完全獨立，兩核只透過緩衝區交換資料，互不干擾。**

由這條目標推出的硬規則：

1. ❌ **唔准跨核 notify**（`xTaskNotifyGive` 去另一核嘅 task）
2. ❌ **唔准跨核 queue 有阻塞嘅等待者**
3. ❌ **唔准 unpinned task**（unpinned = 調度器可以隨時搬去另一核）
4. ✅ 跨核交換一律用 **`nc_hub`**：SPSC、無鎖、**永不等待、永不喚醒**，
   `write_begin` 冇空位回 NULL、`read_begin` 冇 READY 回 NULL，呼叫端自己跳過一輪。

### 2.1 現存違規（待修）

| # | 違規 | 位置 | 修法 |
|---|---|---|---|
| 1 | 跨核 notify：`tcp_nc4_rx`(core 1) → `nc4_bus_kick()` → `decode0`(core 0) | `nc4_bus.c` `nc4_bus_kick()` | 刪 kick，consumer 1 ms 輪詢（或 `s_rx_split=false` 同核，已係預設） |
| 2 | `httpd`(prio 10) unpinned | `nc4_ws.c` `httpd_start` | 釘 core 0 |
| 3 | lwIP tcpip(prio 18) unpinned | `CONFIG_LWIP_TCPIP_TASK_AFFINITY=0x7FFFFFFF` | 釘 core 0 |
| 4 | TinyUSB task 喺 core 1 | `TINYUSB_DEFAULT_TASK_AFFINITY`（multicore = 1） | 改 0 |
| 5 | `usb_reconn`(prio 5) unpinned | `nc_store.c` | 釘 core 0 |
| 6 | bench hub 跨核 writer/reader | `main.c` 的 `hub_bench_*` | 屬測試，隨測試一起移走 |

### 2.2 修完之後剩低的干擾

| 干擾 | 範圍 | 可否避免 |
|---|---|---|
| Tick 1 kHz | 兩核 | ❌ 接受 |
| 自己核上的 ISR | 該核 | 你 core 1 冇任何自裝 ISR |
| Flash 寫入／擦除 | **兩核停頓 ms 級** | ❌ unavoidable（USB MSC 令它隨時可達） |
| heap mutex | 跨核 | ✅ 熱路徑唔准 `malloc` |
| **console log 鎖** | **跨核** | ⚠️ 見第 3 節 |
| esp_timer | 只 core 0 | ✅ 與 core 1 無關 |

---

## 3. 等待：三種形態，只有一種禁止

> **等待唔係問題；「唔知幾時返嚟，而又燒住 CPU」才係問題。**

| 形態 | 例子 | 規則 |
|---|---|---|
| **硬等／自旋** | `while(!flag);`、`for(;;)` 空轉、輪詢 busy loop | ❌ **完全禁止** |
| **定時阻塞** | `vTaskDelay()`、`xTaskDelayUntil()` | ✅ **允許**：交還 CPU、時間已知、到期自動回隊列 |
| **條件阻塞** | queue／notify／mutex／socket | ⚠️ 允許，**只限 I/O 邊界**，且一定要有 timeout |

- 禁止的定義：**等待期間 CPU 仍然 100% 屬於該 task**。
- 允許的定義：**等待期間 CPU 交還，到期（或事件到）由 FreeRTOS 重新排入 ready list**。
- 定時阻塞係唯一合法的「唔做嘢」手段；冇它的話個核會 100% 燒電。

### 3.1 現存例外（要明確標記，唔可以靜靜存在）

| 位置 | 形態 | 處置 |
|---|---|---|
| `main.c` `tcp_nc4_rx_task` 的 `if (++spins < 300) taskYIELD();` | 有界自旋 | 改成 `vTaskDelay(1)` 會令小幀吞吐 6.2 → 0.78 MB/s。**待拍板**：守規則，或開一個明寫的例外 |

**規則**：任何例外必須寫在本節，並註明數字與原因。唔准有未記錄的自旋。

### 3.2 大循環（round / epoch）已停用

逐圈量測（`nc_epoch`）**不再使用**：週期由最慢那條 loop 決定，數字意義有限，而干擾來源
（事件到達率）已經清楚。移除掛載點：`nc_task_loop.c` 的兩處 `nc_epoch_mark`、
`nc_epoch_add/remove`、STATUS 的 `core0/core1` 段。`nc_epoch.c` 保留但不掛載。

未來的效能評估方式：**乾淨環境下做壓力測試**，不由管理器在線量測。
（若日後要工具，另開一支獨立小工具，唔好塞回 trampoline。）

---

## 4. Log：熱路徑零 log，彙總後一次輸出

1. **熱路徑零 log**（每個 loop 內、每幀內、每個封包內）。
2. **統計用 counter 累積**（`s_rx_frames++` 這種），唔用 log。
3. **一次輸出**：由 `STATUS_GET` 拉，或由**專門的 print 任務**統一印。

**為甚麼**：`ESP_LOGI` 走 vfs → 驅動 → **有 mutex**。core 1 打印會等 core 0 打印完
⇒ console 是跨核耦合，直接違反第 2 節。

**print 任務**（未來）：一條獨立、低 prio、**可以完全關閉**的任務；其他 loop 只寫 ring／counter，
由它負責格式化與輸出。舊專案就是這樣做，效果比現在的散點 log 好。

---

## 5. 緩衝與堆疊：宣告 → 匯總 → carve → 發

```
① task 自己講要幾多（declare）
② 管理器加總、排序、逐項驗證
③ 每個 class 一次 carve（32 B 對齊）
④ 逐項發指標；失敗的項回 NULL，由呼叫端自己處理
```

- 這**唔係**「管理器計算」，係「管理器匯總＋分配＋驗證」。需求永遠由 task 講。
- 現況：`nc_plan` 已實作①–④，`avail` 已用實測 `heap_caps_get_largest_free_block`。
- 缺：**task 講的數字仍然係硬編碼常數**（`.stack = 6144` 等）。
- 補法：把 `uxTaskGetStackHighWaterMark()` 納入 STATUS；用實測值調 `.stack`，唔靠猜。
- 規則：熱路徑唔准 `malloc`；開機 carve 的緩衝**由開機用到關機，唔 free**。

---

## 6. 測試歸類（唔准污染功能結構）

```
firmware/components/action/
├── file_action/      真功能
├── ota_action/       真功能
└── test_action/      測試指令（0xF00x / 0x181x）—— **已搬，2026-11-04**
firmware/components/tasks/
├── test_bench_tasks/      bench_a/b/c（量測驗證）
├── test_hello_tasks/      hello（框架自測）
├── test_fx_tasks/         fx（POC 燈效引擎；運作方式未定）
├── test_probe_tasks/      probe（開機 hub 自測）
└── test_tcp_tasks/        tcp_sink0/3、tcp_src0/1（P4 吞吐量測，5000–5003）

（monitor 已刪：2026-11-04，只有一行 log，價值唔夠 3 KB + 跨核讀取）
```

**現況（2026-11-04）**：

- `components/tasks` 整個目錄由 `NC_ENABLE_TESTS` gate；OFF = 唔加入 `EXTRA_COMPONENT_DIRS` = 唔編譯、唔 link。
- `fx`／`monitor` 係 POC／實驗，所以同測試放埋一齊（release 唔會出）。
- 全部已搬完（2026-11-04）：`test_action`（8 條測試指令）、`test_probe_tasks`、`test_tcp_tasks`
  （`tcp_sink/src`）；`hub_w/r`、`tx_pack` 隨 `HUB_BENCH`／`TX_BENCH` 一齊入 `test_action`。
- `main.c` 只剩 5 條真指令（`REBOOT`／`STATUS_GET`／`ROUTE_GET`／`SET_MASTER`／`SYS_TASK_QUERY`）。

**指令分類（15 條 → 7 真 + 8 測試）**

| 真功能 | 測試 → `test_action` |
|---|---|
| `REBOOT`、`STATUS_GET`、`ROUTE_GET`、`SET_MASTER`、`SYS_TASK_QUERY`、`BUS_STATS`、`NET_TUNE` | `DEV_BENCH`、`HUB_BENCH`、`TX_BENCH`、`BENCH_READY`、`BENCH_DATA`、`BENCH_RESULT`（`DEV_SLOW`／`UART_BENCH` 已刪） |

**編譯開關**（真正「唔污染」的做法：唔係靠命名，係靠唔存在）：

```cmake
option(NC_ENABLE_TESTS "測試指令與測試任務" OFF)  # 預設關；要量測時 -DNC_ENABLE_TESTS=ON
```

關掉之後 `test_action`／`test_*_tasks` 唔編譯、唔註冊、唔佔 stack。

**文件規則**：測試只准出現在 `docs/02_guides/testing.md` 與 `docs/03_notes/test-*.md`；
`docs/00_pm/roadmap.md` 只准一行。

---

## 7. 裁決：`slow_worker` 刪除

**原本理由**：NC4 指令由 decode task 執行，慢指令（UART bench、模擬寫 flash）會令整條解碼線停，
所以要一個 `nc_hub` + worker 做 deferred execution。

**這個理由唔成立**：指令層面是**嚴格一問一答**——一條指令未執行完，唔會發下一條
（網絡傳輸亦然）。所以 decode 永遠只會持有一條指令，唔會出現「被慢指令擋住後面的指令」。
「慢工作阻塞」在指令層面**天然唔存在**。

**裁決**：**刪除** `slow_worker` 及它的兩個測試 job（`UART_BENCH`、`DEV_SLOW`）。

- 唔需要改名（`nc_worker` 不需要存在）。
- 唔需要為它分配緩衝（plan 的 `"jobs"` 8 槽一併刪除）。
- 節省：4 KB stack、每秒 1000 次喚醒（約 0.5% CPU）、8 格 hub。

**將來真正的慢工作**（flash 寫入、FAT、OTA）用**專屬 task**：一個用途一條 task，
唔用通用 worker。通用 worker 會令「邊個喺邊個核做咩」變模糊，正好違反第 2 節的隔離目標。

### 7.1 執行紀錄（2026-11-04）

✅ **已執行**：

| 移除 | 位置 | 連帶 |
|---|---|---|
| `slow_worker` loop | `main.c` `s_core_loops` / `s_task_core` | 任務管理器唔再註冊 `core` 任務 |
| `slow_job_t` / `s_job_hub` | `main.c` | plan 的 `"jobs"`（8 槽）一併刪 |
| `0xF005 DEV_SLOW`、`0xF006 UART_BENCH` | `main.c` handler + 指令表 | `s_core_actions[]` 由 15 → 13 條 |
| `task_core_declare` / `task_core_start` | `main.c` | — |

**順手修好一個真 bug**：`uart_loop_init()` 以前只由 `UART_BENCH` 呼叫，
所以 **UART 通道要先打一次測試指令才會通**。現在改由 `task_net_start()` 在開機時安裝
（只在 `link_enabled("UART")` 且有 `uart.rx` 緩衝時），baud 用 `UART_TEST_DEF_BAUD`（921600）。

驗證：S3／P4 零 warning，flash −3,936 B（S3）／−4,992 B（P4）；hosttest 8/8。
