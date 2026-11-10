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
|---|---|---|---|---:|---|
| ~~1.1~~ | ~~P4 測試 task 開關（sink×2／src×2）~~ | ✅ **24,576 B** | — | 已做：`CONFIG_NETCORE_TEST_TASKS` 預設 n；accept 係正式通道 |
| ~~1.2~~ | ~~`0x1005` 768 B 截斷修正~~ | ✅ 0 | — | 已做：`nc_task_list_json()` 緊湊清單，保證合法 JSON |
| ~~1.3~~ | ~~round watchdog~~ | ✅ +3,072 B runtime | — | 已做：`monitor` 任務（core0／1 s／`no_epoch`），只記錄 |
| ~~0.1~~ | ~~死碼清理（`NC_DECODE_CORE`／`nc4_bus_start`／`0x1019`／重複 include／`DEV_BENCH` mode 2/3/4）~~ | ✅ 0 行為改動 | — | 已做：S3／P4 零 warning、hosttest 8/8 |
| **2.1** | P4 排除 tinyusb（`nc_store` MSC） | **21.7 KB 靜態** | 中 | 本板冇 OTG（晶片有）→ MSC 用不到；要動 store |
| **2.2** | `ssid_pw` 搬 NVS | 0 | 低 | 要決定 NVS key 格式 |
| **3.1** | `links[]` 其餘欄位（region/chunk/budget）接 config | 0 | 中 | 要先定 schema |
| **3.2** | 狀態管理器（三層＋字典） | +256 B | 中 | 設計已定，未實作 |
| ~~3.3~~ | ~~per-core decoder~~ | ✅ 0 | — | 已做（§7.26）：`s_decoder[2]`，每核一把鎖／一個回覆幀 |
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

## 6. 2026-11-08：發送／轉寄路徑（**已落地**，剩「4 KB 環」與政策）

> 依據：[tx-path-design.md](../03_notes/tx-path-design.md)、[status §7.28](status.md)。
> 2026-11-08 晚完成 6.1／6.3／6.4 ＋ UART TX（沿用 1 KB 環）；上板驗證通過。

| # | 項目 | 狀態 | 大小／風險 | 備註 |
|---|---|---|---|---|
| 6.1 | 回覆超限不再靜默截斷 | ✅ 完成 | 0 | 回 `{"err":"reply_too_big","need":N,"cap":2600}` |
| 6.2 | UART `send` 接上 | ✅ 完成（1,024 環） | **0 RAM** | 4 KB 轉寄會阻塞 ~44 ms @921600 |
| 6.2b | UART TX ring → 4,192 | ⏸ **待你決定** | **+3.2 KB 內部** | 只有「要轉寄 4 KB 進 UART」才需要 |
| 6.3 | `nc4_link` ＋ `nc4_link_send` | ✅ 完成 | +~1.6 KB 靜態 | 整幀交出去；短送／錯誤計 drops |
| 6.4 | router `out` 支援 link 轉寄 | ✅ 完成 | 0 | 先轉寄、後本地；forward-only 仍未支援 |
| 6.5 | 量測 | 部分 | — | 21 B 幀已量；4 KB／TCP 8 KB／OTA 停頓未量 |

**明確不做**：S3 的 8 KB 轉寄（沒有需求；8 KB 留給 P4 TCP，插槽已有）。

**仍未做（依序）**

1. **UART 環 4,192**（+3.2 KB 內部）—— 要 4 KB 轉寄才做；現在 1 KB 覆蓋命令與回覆。
2. **轉寄滿了的政策**：目前沿用各 transport（CDC 100 ms 後丟、UART 排空、TCP 等到 ACK）。
   若要「立即丟＋計數」就要為 CDC／TCP 加非阻塞寫入（TCP 需 `MSG_DONTWAIT` ＋ 續傳）。
3. **forward-only route**（`out:["UART"]` 冇 `decodeN`）：核歸屬規則不變，這種行仍然唔建立通道。
4. **環狀拓撲**（`CDC→UART` 與 `UART→CDC` 互指）未擋；自我反射已濾，但兩條線互指會無限轉。
5. **OTA flash 停頓 vs `uart.rx`**：每核 decoder 之後冇「意外保護」，仍未處理。

**同日順手修的兩件基礎設施**

- `make.py flash --tests`：燒錄那一步冇把 `tests` 傳給 `idf.py` → 先建測試映像、再在
  `app-flash` 時悄悄重編成 release 才寫入。已修 `tools/ncbuild/builder.py`。
- **指令碼重複**：原本排在 qsort 之後 dedupe，相等 key 的次序冇保證 → 同一個 cmd
  可以在兩顆 binary 指向不同 handler。改成「填入時先到先得＋`ESP_LOGE`」。
  今天真的撞到：0xF012／0xF013 已被 SD bench 佔用，新測試指令改用 **0xF01A／0xF01B**。

## 7. 2026-11-09：共用緩衝池、跨核轉寄、PSRAM 實測（**已落地**）

> 依據：[buffer-pool.md](../03_notes/buffer-pool.md)、[status §7.29](status.md)。

| # | 項目 | 狀態 | 大小／風險 | 備註 |
|---|---|---|---|---|
| 7.1 | `nc_pool`（small／bulk／forward） | ✅ 完成 | 28,992 B ＋ descriptor ~1.5 KB | `STATUS mem.pool` 看得到 used/peak/fails |
| 7.2 | `nc4.pool` 設定 | ✅ 完成 | 0 | S3／P4 system.json；`where` internal／psram |
| 7.3 | 跨核轉寄 queue | ✅ 完成 | queue 8 幀／核 | 目的核 `nc4_fwd_poll`；`net.fwd` 統計 |
| 7.4 | `0xF01C MEM_BENCH` | ✅ 完成 | 測試映像 | internal 240／PSRAM 21 MB/s（copy） |
| 7.5 | forward 成功路徑 | ⏸ 待 UART 腳位 | — | 現時 UART 被 SD GPIO7/8 佔用，測到 `err` 而非 `frames` |
| 7.6 | TTL／leak watchdog、tap | ⏸ 未做 | — | refcount 已在，未有 subscriber |
| 7.7 | pool 接到 RX（header-first assembler） | ⏸ 未做 | 大改 | 現況仍用 per-link parser ring／PUSH slots |
| 7.8 | 多實例（UART0/1/2、I2C、SPI） | ⏸ 未做 | 大改 | 要 pin map 與 per-instance owner |
| 7.9 | `where=psram` 上板 override | ⏸ 未測 | — | USB volume 在 app 模式冇重新掛載；hosttest 已驗解析 |

## 8. 2026-11-09：通訊緩衝定案（設計完成，P1 待實作）

> 依據：[comm-buffers.md](../03_notes/comm-buffers.md)、[status §7.30](status.md)。

| # | 項目 | 狀態 | 備註 |
|---|---|---|---|
| 8.1 | RX 留 internal | ✅ 定案 | per-link parser／PUSH slots 不變 |
| 8.2 | 轉發走 PSRAM `nc_hub` | ✅ 定案 | per（目的 link × 來源核）；strict FIFO；writer mutex |
| 8.3 | `nc_pool.forward` 取消 | ✅ 定案 | small／bulk 預設關；等 header-first／OTA |
| 8.4 | `nc4_fwd` hub 版 | ⏸ **P1 待做** | 保留舊 queue `mode=pool` 做 A/B |
| 8.5 | `nc4.forward` config | ⏸ P0 待加 | `where=psram`、`depth=2`、`max_frame` |
| 8.6 | tap／refcount | ⏸ P3 | 用 `nc_hub_generation` 防 ABA |
| 8.7 | P4 8K／depth 4 | ⏸ P4 | slot 8,256；量 PSRAM 頻寬 |
