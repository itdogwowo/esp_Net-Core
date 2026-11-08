# 下一步計劃（2026-11-04 晚）

> **依據**：[handoff §5／§6](handoff-2026-11-04.md)、
> [execution-model §8](../03_notes/execution-model.md)、
> [status §7.2](status.md)、[memory-budget §9.5](../03_notes/memory-budget.md)。
> P0（`router_from_cfg` UAF）已完成。
> **Phase 1（1.1／1.2／1.3）已完成**：測試 task 開關、`0x1005` 修正、round watchdog。
> **死碼清理已完成**（0.1）：見 [disposition](disposition.md) B2b／B3／B4／B5／B6，行為不變。
> 剩下的 Phase 2 卡在一個 USB 事實確認（見 §3）。

---

## 0. 先修正一條錯誤建議：`NC_TASK_MAX 20 → 12` 不可行

之前的文件寫「縮到 12 格可省 ~3 KB」。實際數：

- 固定註冊 **10** 個任務（nc4、net、core、monitor、probe、fx、hello、bench_a/b/c）
- `.fxe` 最多 attach **8** 個（`NC_FX_MAX_MODULES = 8`）
- **10 + 8 = 18**

所以 20 格是**必要的**（還要留 3 格緩衝）。縮到 12 會在載入第 4 個 `.fxe` 之後
`nc_task_attach()` 直接回 `ESP_ERR_NO_MEM`，而且是**靜默的功能消失**。
→ **不做**，並已修正三份文件裡的說法。

---

## 1. 排序

| 階段 | 項目 | 大小 | 風險 | 依賴／決策 |
|---|---|---|---:|---|
| ~~1.1~~ | ~~P4 測試 task 開關（sink×2／src×2）~~ | ✅ **24,576 B** | — | 已做：`CONFIG_NETCORE_TEST_TASKS` 預設 n；accept 係正式通道 |
| ~~1.2~~ | ~~`0x1005` 768 B 截斷修正~~ | ✅ 0 | — | 已做：`nc_task_list_json()` 緊湊清單，保證合法 JSON |
| ~~1.3~~ | ~~round watchdog~~ | ✅ +3,072 B runtime | — | 已做：`monitor` 任務（core0／1 s／`no_epoch`），只記錄 |
| ~~0.1~~ | ~~死碼清理（`NC_DECODE_CORE`／`nc4_bus_start`／`0x1019`／重複 include／`DEV_BENCH` mode 2/3/4）~~ | ✅ 0 行為改動 | — | 已做：S3／P4 零 warning、hosttest 8/8 |
| **2.1** | P4 排除 tinyusb（`nc_store` MSC） | **21.7 KB 靜態** | 中 | 本板冇 OTG（晶片有）→ MSC 用不到；要動 store |
| **2.2** | `ssid_pw` 搬 NVS | 0 | 低 | 要決定 NVS key 格式 |
| **3.1** | `links[]` 其餘欄位（region/chunk/budget）接 config | 0 | 中 | 要先定 schema |
| **3.2** | 狀態管理器（三層＋字典） | +256 B | 中 | 設計已定，未實作 |
| **3.3** | per-core decoder | 可能更多 | 高 | 要先決定「一個 decoder 還是兩個」 |
| **3.4** | `group`（局部 B） | 省 stack | 中 | 等低頻 loop 多起來才做 |

---

## 2. Phase 1 細節

### 2.1 測試 task 開關（P4）

- 位置：`main.c` ETH 起來之後的 5 個 `xTaskCreatePinnedToCore`。
- 做法：`CONFIG_NETCORE_TEST_TASKS`（`main/Kconfig.projbuild`，預設 **n**）；
  關掉時不建立 task、也不編那些函式（`tcp_sink_task` / `tcp_src_task`）。
- 影響：`tools/bench/bench_all.py` 之類會用到 5000–5003 的腳本要**手動開**才有效。
- 驗收：P4 `idf.py size` 不變（runtime 而非靜態）；開機 log 顯示 `test tasks: off`；
  `epoch` 的 core0/1 在沒有測試 task 後仍然正常（rounds 繼續增加）。

### 2.2 `0x1005` 截斷

- 現況：`char json[768]`，實際 >1.5 KB → 回半截 JSON。
- 做法：改成走「回覆幀 + 有界 writer」，放不下就回合法的 `{"more":1,"next":...}`；
  或者直接叫主機用 `STATUS_GET keys=tasks`（但要保留 `0x1005` 的相容性）。
- 驗收：`0x1005` 回覆一定是合法 JSON（host 端 json.loads 成功）。

### 2.3 round watchdog

- 每核用 `nc_epoch` 的 `rounds` 判斷停滯：
  `collecting && rounds 在 N 秒內沒有增加` → `ESP_LOGE` + 印當下 `epoch` 與各 loop 的
  `count/last_us`（從 `nc_task_json` 取）。
- 預設**只記錄**；`NC_MON_RESTART_S > 0` 才 `esp_restart()`。
- 放在一個 SYSTEM 任務 `monitor`（level 2、core 0、period 1000 ms），不塞進 slow_worker。
- 驗收：故意把一條 loop 卡住（測試用 `0xF005`），watchdog 要在 N 秒內印出正確的 loop。

---

## 3. Phase 2／3（先不動）

### 3.0 先確認：P4 的 USB 到底有沒有

| 事實 | 來源 |
|---|---|
| **晶片有** USB 2.0 OTG（2 個 OTG peripheral、UTMI＋FS/LS PHY） | `soc_caps.h`：`SOC_USB_OTG_SUPPORTED=1`、`SOC_USB_OTG_PERIPH_NUM=2` |
| **這塊板** 的 USB-C 只接 USB-Serial-JTAG：`"usb": false` | `ports/P4/ESP32-P4-ETH/board.json` |
| 實測：TinyUSB 從未列舉成功；console 必須 UART0（USB-Serial/JTAG console 會令 bootloader 卡死） | [p4-bringup.md](../03_notes/p4-bringup.md)、[failure-policy.md](../03_notes/failure-policy.md) |
| 目前 firmware 在 P4 **執行期**已停用 TinyUSB（`err = ESP_ERR_NOT_SUPPORTED`），但 `nc_store` 仍然 link MSC | `main.c`、`nc_store.c` |

**結論**：不是晶片沒有 OTG，是**這塊板**沒有把 OTG 拉出來。
所以排除 tinyusb 應該用**板子能力**（`board.json` → Kconfig），不要用 `IDF_TARGET`：
將來的 P4 板如果有 OTG，只要把能力打開就可以，不用改 code。

- **2.1 tinyusb**：`nc_store.c` 的 MSC 段用 `#if !CONFIG_IDF_TARGET_ESP32P4` 包起來；
  `nc_store_release_usb()` 在 P4 回 `ESP_ERR_NOT_SUPPORTED`；狀態 JSON 的 `storage.mode`
  仍然要有意義（永遠 `app`）。風險：store 是 FAT 擁有者，改動要連帶看 `nc_store_app_ready()`。
- **3.x**：等 Phase 1／2 完成、上板驗證過再排。

---

## 4. 需要你決定的事

1. ~~測試 task 預設~~ → **n**（已實作；bench 要量時手動開）
2. ~~watchdog 停滯時~~ → **只記錄**（已實作；`NC_MON_RESTART_S` 可覆蓋）
3. **tinyusb 排除**：見 §3（要先確認板子的 USB 接線）

---

## 5. 建議

**今天先做 1.1 → 1.2 → 1.3**（都是低風險、可獨立驗收），
上板確認後再做 2.1（tinyusb）。
3.x 是架構級，等記憶體與執行模型的驗證數據出來再談。

---

## 6. 2026-11-08 新增：發送／轉寄路徑（設計已定，未實作）

> 依據：[tx-path-design.md](../03_notes/tx-path-design.md)。**先做 6.1、6.3（低風險），6.2 看你決定。**

| # | 項目 | 大小 | 風險 | 備註 |
|---|---|---|---:|---|
| 6.1 | 回覆超限不再靜默截斷（明確錯誤或分頁） | 0 | 低 | 目前 >2,600 B 會靜默截斷 |
| 6.2 | UART TX ring 1,024 → 4,192 ＋ `send` 接上 | **+3.2 KB 內部** | 中 | 先決定 UART 要不要 TX |
| 6.3 | `link_send(dest, ptr, len)`：能塞就 copy 返回、塞不下才等（背壓） | 0（API） | 中 | 轉寄與大回覆共用 |
| 6.4 | router `out` 支援 link 轉寄（走 6.3，不經回覆幀） | 0 | 中 | 4 KB 上限即可 |
| 6.5 | 量測：4 KB 轉寄時 decode 佔用、TCP 8 KB 阻塞、OTA flash 停頓對 UART | 0 | 低 | 見 tx-path-design §8 |

**明確不做**：S3 的 8 KB 轉寄（沒有需求；8 KB 留給 P4 TCP，插槽已有）。

**要你決定**：① UART 要不要 TX？② 轉寄滿了是背壓還是丟棄＋計數？③ 8 KB 轉寄要不要納入需求？
