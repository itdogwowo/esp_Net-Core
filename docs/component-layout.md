# esp_Net-Core — 分層、component 與專案佈局

> 狀態：定案（對應 `rewrite-plan.md` Phase 1 的專案骨架）
> 原則：**程式分七區、專案分 ports；箭頭只能向下。**
> 相關：`architecture-flow.md`（執行期流程）、`rewrite-plan.md`（分期）

---

## 1. 七個程式分區

| 分區 | 一句話 | 知道什麼 | 可以依賴 |
|---|---|---|---|
| **hw** | 硬件 | 暫存器、時序、匯流排 | 只有 `esp_driver_*` |
| **sys** | 系統 | 協議、設定、FS、匯流排、節拍 | hw、`esp_*` |
| **sw** | 軟件 | 效果引擎、layout、math（功能實作） | sys |
| **action** | 政策 | 參數怎麼驗、選哪個機制、回什麼 | sys、sw |
| **tasks** | 迴圈 | 誰在什麼核心上、跑多久 | sys、sw（觸發指令走 service） |
| **ui** | 介面 | 頁面、觸控 | sys、sw（觸發指令走 service） |
| **schema** | 契約 | 指令表（build 時生成） | 無 |

三條機械規則：

1. `hw` 不准 include `sys` / `sw` / `action`。
2. `action` 不准被 `sys` / `sw` include（反向觸發走 `disp` service）。
3. `ui` 與 `tasks` 不准 include `action`（同上，走 service）。

---

## 2. 依賴方向

```text
                        main
                          │
        ┌─────────────┬───┴───┬─────────────┐
        ▼             ▼       ▼             ▼
      tasks          ui    action         (裝配)
        ╎             ╎       │
        ╎ (service)   ╎       ▼
        └─────────────╊──►   sw ──► sys ──► hw
                      ╎
              ui ↔ action 走 disp service，不互相 include

schema ──► sys/nc4
```

---

## 3. 目錄

```text
esp_Net-Core/
├─ CMakeLists.txt
├─ main/main.c                     # 只讀起來像裝配清單
├─ components/
│  ├─ hw/      apa102/  pca9685/  motor_uart/  tft/  touch/  xl9555/
│  ├─ sys/     nc4/  config/  store/  bus/  transport/  pipeline/  log/  time/
│  ├─ sw/      effect_core/  pixel_math/  pixel_layout/  pixel_controller/
│  ├─ action/  file_cmds/  pixel_cmds/  sys_cmds/  status_cmds/  ...
│  ├─ tasks/   pixel_task/  net_task/  schedule_task/
│  ├─ ui/      pages/
│  └─ schema/  （生成物）
├─ ports/                          # 專案層（見 §4）
│  ├─ S3/ESP32-S3-Matrix/{data/, sdkconfig.defaults?, partitions.csv?}
│  ├─ P4/…  C6/…
├─ data/                           # 不進 git：用戶 FS 的鏡像（見 §8）
└─ tools/                          # 主機端：golden vectors、PC client
   └─ fixtures/                    # 進 git：測試樣本
```

---

## 4. 專案層：`ports/`

`components/` 分「這是什麼程式」；`ports/` 分「這塊板子要什麼」。兩者不要混。

| 檔案 | 什麼時候需要 |
|---|---|
| `sdkconfig.defaults` | 跟晶片預設不同時（PSRAM、flash 大小、USB 描述符） |
| `partitions.csv` | 分割表跟晶片預設不同時 |
| `data/` | 出廠預置內容（`config.json`、空的 `pixel/` 目錄） |

**晶片 = 一個 build；板子 = 一份 JSON。** S3 的 N 塊板子共用同一個 binary，差異在 `data/config.json`。

### 4.1 預置（「預先放一個進去」）

`data/` 用 IDF 的 FATFS 映像產生器燒進去，不需要寫任何 C：

```cmake
# ports/S3/ESP32-S3-Matrix/CMakeLists.txt
fatfs_create_partition_image(data ${CMAKE_CURRENT_LIST_DIR}/data FLASH_IN_PROJECT)
```

需求：分割表要有一個 `data` 分區（type data / subtype fat）。

⚠️ **`idf.py flash` 每次都會重燒這個映像**，把使用者改過的 `config.json` 蓋回出廠版。所以：

- 開發／日常 → `idf.py app-flash`（只燒 app，不動 data）
- 出廠／要重置 → `idf.py flash`

### 4.2 邊界

- `ports/<board>/data/` 只放**出廠預設**。燒完之後那份內容屬於使用者。
- 預置不是保證：使用者可以刪掉 `config.json`。**缺檔必須能開機**（用 firmware 內建的預設值），不能因此進不了 RUN。

---

## 5. 與既有目錄的對照

| 既有 | C 版 |
|---|---|
| `lib/hw/` | `components/hw/` |
| `lib/sys/` | `components/sys/` |
| `lib/sw/` | `components/sw/` |
| `action/` | `components/action/` |
| `tasks/` | `components/tasks/` |
| `ui/` | `components/ui/` |
| `schema/*.json` | `components/schema/`（build 時轉 C 表） |
| `pixel/` | **消失**：程式在 `sw/`，資料在 FAT |
| `cores/` | 併入 `tasks/`（core pin 表） |
| `ports/<chip>/<board>/config.json` | `ports/<chip>/<board>/data/config.json` ＋ 可選 `sdkconfig.defaults` |
| `slave/config.json`（base） | firmware 內建預設值（找不到檔案時用） |

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

---

## 9. Partition 佈局

| 分區 | 內容 | runtime 誰能寫 |
|---|---|---|
| `factory` / `ota_0` / `ota_1` | firmware（含 schema 表） | 只有 OTA |
| `data`（FAT） | `config.json`、`pixel/` | host（MSC） |
| `nvs` | cID、`master_cid`、`next_mode`、boots 計數 | 程式 |

firmware 走 OTA 分區 → 舊版 `FILE_PROMOTE` 那套（`.bak` ＋ pending ＋ `boots>=3` 自動回滾）**只保留給 assets**。

---

## 10. 判準（review 時逐條問）

- [ ] 這個改動是「硬體變了」「節拍變了」「內容變了」還是「政策變了」？（決定分區）
- [ ] 箭頭有沒有反向？（`hw` → 上層、`sys` → `action`、`ui`/`tasks` → `action` 都是錯的）
- [ ] `main.c` 有沒有出現 if / for / 字串解析？
- [ ] `action` 的 handler 有沒有超過「驗參數、選機制、回結果」？
- [ ] 新增的設定，是專案（`ports/`）、執行期（`data/`）還是機器狀態（NVS）？
- [ ] 放進隨身碟的檔案，會不會改變協議行為？（會 → 它不該在 `data/`）
