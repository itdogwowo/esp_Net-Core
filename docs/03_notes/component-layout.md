# esp_Net-Core — 分層、component 與專案佈局

> 狀態：2026-11-19 更新（原 2026-10 定案）
> 原則：**程式按角色分區、專案分 ports；箭頭只能向下。**
> **角色 → 目錄 → 命名的權威表在 [behavior-taxonomy.md](behavior-taxonomy.md)**；本頁只講分區與依賴方向。
> **CORE 只是三個平面之一**：CORE（`firmware/components/`）／USER（repo 根 `modules/`）／
> DATA（裝置 `/data/`）。三平面規則見 behavior-taxonomy.md §2；本頁只講 CORE 內部。
> 相關：`architecture-flow.md`（執行期流程）、`../00_pm/roadmap.md`（分期）

> **現況（2026-11-19）**：已落地 `driver/`、`bus/`、`sys/`、`sw/`、
> `effects/`（目錄已立、效果 M2 才遷入）、`tasks/`、`action/`、`host/`；
> `hw/`（具體晶片）、`ui/`、`schema/` 未落地。
> 看目錄時以 §3 的 ✅ / 🟡 / ⬜ 標記為準。

---

## 1. 程式分區（按角色）

| 分區 | 一句話 | 知道什麼 | 可以依賴 |
|---|---|---|---|
| **driver** | 周邊驅動 | 暫存器、時序、腳位 | 只有 `esp_driver_*`／`soc` |
| **hw** | 晶片 | 一顆具體晶片的寄存器序列／封包（用 bus 讀寫） | bus |
| **bus** | 線路 | 一條線的統一讀寫語義 | driver |
| **sys** | 系統 | 協議、設定、FS、記憶體、任務、節拍 | driver、bus、`esp_*` |
| **sw** | 共用工具 | 環、編碼、抖動、layout、**效果引擎**（registry／interpreter；**唔養行為、唔放具體效果**） | sys |
| **effects** | 效果實作 | （時間, 參數, 幾何）→ canonical 畫面（引擎在 `sw/pixel/engine/`） | sw（只 include 契約 header） |
| **action** | 政策 | 參數怎麼驗、選哪個機制、回什麼 | sys、sw、effects（只設參數） |
| **tasks** | 迴圈 | 誰在什麼核心上、跑多久 | sys、sw、effects（＋後端轉接例外） |
| **host** | 宿主 | 載入並執行外部模組（ncm／WAMR） | sys |
| **ui** | 介面 | 頁面、觸控 | sys、sw（觸發指令走 service） |
| **schema** | 契約 | 指令表（build 時生成） | 無 |

五條機械規則：

1. `driver` 不准 include `hw` / `bus` / `sys` / `sw` / `effects` / `tasks` / `action`；
   `hw` 不准 include `sys` / `sw` / `effects` / `tasks` / `action`。
2. `effects` 不准 include `tasks` / `action` / `driver` / `bus` / `host`；唔准開 task、唔准配置記憶體。
3. `action` 不准被 `sys` / `sw` include（反向觸發走 `disp` service）。
4. `ui` 與 `tasks` 不准 include `action`（同上，走 service）。
5. 同層唔互相 include；要共用就落 `sw/`。

---

## 2. 依賴方向

```text
                        main
                          │
        ┌─────────────┬───┴───┬─────────────┐
        ▼             ▼       ▼             ▼
      tasks          ui    action         host
        ╎             ╎       │             │
        ╎ (service)   ╎       ▼             │
        └─────────────╊──► effects ──► sw ──► sys ──► bus ──► driver
                      ╎                          ▲
              ui ↔ action 走 disp service         │
              host 經 sys 載入模組 ────────────────┘（外掛效果係檔案，唔入固件）

   tasks／action ──► hw（晶片）──► bus ──► driver（晶片用線路；上層用晶片，唔可以反向）
schema ──► sys/nc4
```

---

## 3. 目錄

✅ = 已落地　⬜ = 目標，還沒建立

```text
esp_Net-Core/
├─ docs/                              # 文件層：00_pm / 01_protocol / 02_guides / 03_notes
├─ firmware/                          # ★ ESP-IDF 工程根（idf.py 在這裡跑）
│  ├─ CMakeLists.txt                  #   PORT 選擇、sdkconfig 疊層、EXTRA_COMPONENT_DIRS
│  ├─ main/main.c                     #   ✅ 目前 = 裝配 + 尚未拆出的 handler
│  ├─ components/
│  │  ├─ driver/                      # ✅ drv_uart/ drv_i80/ drv_spi_led/ drv_pwm/
│  │  ├─ hw/                          # ⬜ 具體晶片（TFT／PCA9685／XL9555）；等第二顆晶片才建
│  │  ├─ bus/                         # ✅ bus_core/ bus_net/ bus_uart/
│  │  ├─ sys/                         # ✅ nc4/ mem/ config/ task/ store/ hw/ state/ nclog/ core/
│  │  ├─ sw/                          # 🟡 pixel/（環＋編碼＋engine/；ring/·encode/ 仍平放）
│  │  ├─ effects/                     # ✅ pixel_effects/（5 個效果實作＋登記表）
│  │  ├─ action/                      # 🟡 ota_action/ file_action/ test_action/（dev）
│  │  ├─ tasks/                       # 🟡 hw_gpio/、pixel_effect_task/、pixel_pair_task/、pixel_wave_task/、render_task/（公用輸出層）
│  │  ├─ host/                        # 🟡 ncm/（模組載入＋WAMR）
│  │  ├─ ui/                          # ⬜ pages/
│  │  └─ schema/                      # ⬜ （生成物）
│  ├─ sdkconfig.defaults              #   晶片層建置配置
│  └─ partitions.csv                  #   分割表
├─ ports/                             # 專案層（見 §4）
│  ├─ S3/ESP32-S3-N16R8/              #   data/config/*.json
│  └─ P4/ESP32-P4-ETH/                #   sdkconfig.defaults、data/config/*.json
├─ tools/                             # 主機端：board.sh、clients、bench/、hosttest/、poc/
│  └─ poc/mod_poc/                     #   效果模組 ABI 的 host 參考實作
└─ data/                              # 不進 git：用戶 FS 的鏡像（見 §8）
```

---

## 4. 專案層：`ports/`

`components/` 分「這是什麼程式」；`ports/` 分「這塊板子要什麼」。兩者不要混。

| 檔案 | 什麼時候需要 |
|---|---|
| `sdkconfig.defaults` | 跟晶片預設不同時（PSRAM、flash 大小、USB 描述符） |
| `partitions.csv` | 分割表跟晶片預設不同時 |
| `data/` | 出廠預置內容（`config/*.json`） |

**晶片 = 一個 build；板子 = 一組 JSON。** S3 的 N 塊板子共用同一個 binary，差異在 `data/config/*.json`。

### 4.1 預置（「預先放一個進去」）

`data/` 用 IDF 的 FATFS 映像產生器燒進去，不需要寫任何 C：

```cmake
# ports/hardware/S3/ESP32-S3-Matrix/CMakeLists.txt（現在由 firmware/main/CMakeLists 統一處理）
fatfs_create_partition_image(data ${CMAKE_CURRENT_LIST_DIR}/data FLASH_IN_PROJECT)
```

需求：分割表要有一個 `data` 分區（type data / subtype fat）。

⚠️ **`idf.py flash` 每次都會重燒這個映像**，把使用者改過的 `config/*.json` 蓋回出廠版。所以：

- 開發／日常 → `idf.py app-flash`（只燒 app，不動 data）
- 出廠／要重置 → `idf.py flash`

### 4.2 邊界

- `ports/project/<board>/data/` 只放**出廠預設**。燒完之後那份內容屬於使用者。
- `ports/hardware/<board>/wiring.json` 是硬體事實；build 時由 `ncportgen` 編成
  `nc_port.c`（static const）＋ `preset_hardware.json`，不再 runtime 解析。
- 預置不是保證：使用者可以刪掉任何一個檔。**缺檔必須能開機**（該層用內建預設），不能因此進不了 RUN。

---

## 5. 與既有目錄的對照

| 既有 | C 版 |
|---|---|
| `lib/hw/` | `components/hw/`（具體晶片；未落地） |
| `slave/driver/*_drv.py` | `components/driver/drv_*`（周邊／匯流排） |
| `lib/sys/` | `firmware/components/sys/` |
| `lib/sw/` | `components/sw/` |
| `action/` | `components/action/` |
| `tasks/` | `components/tasks/` |
| `ui/` | `components/ui/` |
| `schema/*.json` | `components/schema/`（build 時轉 C 表） |
| `pixel/`（程式） | **拆開**：環／編碼／引擎 → `sw/pixel/`；效果實作 → `effects/pixel_effects/`；資料 → FAT `pixel/`（`effects/{effects.json,*.json,*.nce}`、`map/`、`modes/`、`registry.json`） |
| `modules/effects/*.nce` | repo 根 `modules/effects/`（外掛效果；唔入 firmware 樹） |
| `cores/` | 併入 `tasks/`（core pin 表） |
| `ports/<chip>/<board>/config/*.json` | `ports/project/<chip>/<board>/data/config/*.json` ＋ `ports/hardware/<chip>/<board>/{board,wiring}.json` |
| `config/*.json`（base） | firmware 內建預設值（找不到檔案時用） |

---

## 6. `action` 要薄

`on_file_begin` 15 行——驗參數、選機制、回結果，這是標準形狀。

> handler 只准回答三件事：**參數合不合法、用哪個機制、回什麼**。
> 出現「資料在哪、狀態怎麼存、重試幾次」→ 那是 `sys` 的責任，回去補介面。

反例：`_root_file_exists()` 這種「什麼算根目錄檔」的判斷長在 action 裡，代表 `fs.resolve()` 的語義不夠乾淨，於是每個 handler 各自補一刀。

用這把尺量既有程式：`file_actions` 443 行 / 16 指令 ≈ 27 行，合理；`net_actions` 536、`status_actions` 383 偏厚。

---

## 7. `schema` 是契約，不是資料

| | schema | assets |
|---|---|---|
| 是什麼 | 協議契約 | 內容 |
| 放哪 | **編進 firmware**（build 時生成 C 表） | FAT 分區，host 可改 |
| 誰能改 | 開發者 | 使用者 |
| 改壞的後果 | 編不過 | 保留 last-good |

判準：**能拖進隨身碟的東西，不得決定協議怎麼解。**

---

## 8. 資產不屬於這個專案

`pixel/effects`、`pixel/map`、`pixel/modes` 是**用戶管理的檔案系統**：

- repo 裡沒有 runtime 資產，只有載入器與測試樣本（`tools/fixtures/`）。
- 資產的真相在設備的 FAT 分區。
- 因此資產 schema 是**對外契約**：需要版本欄位、遷移規則、last-good、回到可用狀態的路徑。

現階段只做「載入 + 驗證 + 失敗保留舊版」的骨架；effect program 的 interpreter、mapping 展開、mode 的 range 切片留到之後。

> **唔好同內建效果混淆**：`.nce` 係用戶資產（FAT）；**內建**效果係程式碼，住
> `components/effects/<domain>_effects/`（[behavior-taxonomy.md](behavior-taxonomy.md) §4）。
> 兩者同一個角色、同一個 canonical frame 契約，只差交付方式。

---

## 9. Partition 佈局

| 分區 | 內容 | runtime 誰能寫 |
|---|---|---|
| `factory` / `ota_0` / `ota_1` | firmware（含 schema 表） | 只有 OTA |
| `data`（FAT，卷標 `NETCORE`） | `config/*.json`（第一次開機寫入）、`pixel/effects/*.nce`（選用） | host（MSC） |
| `nvs` | cID、`master_cid`、`next_mode`、boots 計數 | 程式 |

firmware 走 OTA 分區 → 舊版 `FILE_PROMOTE` 那套（`.bak` ＋ pending ＋ `boots>=3` 自動回滾）**只保留給 assets**。

---

## 10. 判準（review 時逐條問）

- [ ] 這個改動是「硬體變了」「節拍變了」「內容變了」還是「政策變了」？（決定分區）
- [ ] 它的**角色**是哪一個？（先查 [behavior-taxonomy.md](behavior-taxonomy.md) §2；答唔到就唔好開檔）
- [ ] 它的**平面**是哪一個？（CORE／USER／DATA；有冇特權？改一次要唔要重燒？）
- [ ] 效果內容有冇住 `effects/`、框架有冇留 `sw/`？有冇偷碰 driver／task／環？
- [ ] 箭頭有沒有反向？（`driver` → 上層、`sys` → `action`、`ui`/`tasks` → `action`、`effects` → `tasks` 都是錯的）
- [ ] `main.c` 有沒有出現 if / for / 字串解析？
- [ ] `action` 的 handler 有沒有超過「驗參數、選機制、回結果」？
- [ ] 新增的設定，是專案（`ports/`）、執行期（`data/`）還是機器狀態（NVS）？
- [ ] 放進隨身碟的檔案，會不會改變協議行為？（會 → 它不該在 `data/`）
