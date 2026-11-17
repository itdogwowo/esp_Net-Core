# firmware/ — ESP-IDF 工程本體

這是唯一會被編進裝置的東西。`idf.py` 的專案根就在這一層。

```bash
# 建議走 tools/board.sh / make.py（它會處理 build dir、target 與 sdkconfig）
../tools/board.sh build ESP32-P4-ETH

# 直接用 idf.py 也可以，但 target、PORT 與 build dir 要自己指定
source ~/esp/esp-idf/export.sh
idf.py -B ../artifacts/build/P4_ESP32-P4-ETH \
       -DSDKCONFIG=../artifacts/build/P4_ESP32-P4-ETH/sdkconfig \
       -DPORT=P4/ESP32-P4-ETH -DIDF_TARGET=esp32p4 build
```

> ⚠️ 每塊板有自己的 `artifacts/build/<chip>_<board>/sdkconfig`。直接用 `idf.py` 而
> 不指定 `-B`／`-DSDKCONFIG` 會退回 `firmware/build` 與 `firmware/sdkconfig`，
> 那就又開始污染專案樹了。

---

## 目錄

```text
firmware/
├─ CMakeLists.txt          工程根：PORT 選擇、sdkconfig 疊層、EXTRA_COMPONENT_DIRS
├─ main/
│  ├─ main.c               裝配 + System 啟動 + 目前還留在這裡的指令 handler
│  ├─ CMakeLists.txt       PRIV_REQUIRES（相依清單）＋ 出廠預置 EMBED_FILES
│  └─ idf_component.yml    外部依賴（esp_tinyusb、cjson）
├─ components/
│  ├─ sys/nc4/             NC4 framing、解碼器、通道框架（nc4.c / nc4_bus.c）
│  ├─ sys/mem/             記憶體原語：pool / arena / plan / 跨核 hub
│  ├─ sys/task/            任務型別 + loop 執行層（建立 / 量測 / 停止）
│  ├─ sys/task_manager/    任務管理器：登記 / 資源 / 開 loop / 狀態 / config 補寫
│  ├─ sys/transport/       TCP 之外的通道：UDP、WebSocket、HTTP 設定端點
│  ├─ sys/store/           FAT 擁有權仲裁（USB ↔ APP、唯讀視圖）
│  ├─ action/ota_action/   OTA 指令（0x22xx）
│  ├─ action/file_action/  檔案查詢與 STATUS_SET（0x2005 / 0x1103）
│  └─ app/fx/              效果載入器（.fxe + manifest + 實例表）
├─ sdkconfig.defaults      共通建置配置（晶片層）
├─ partitions.csv          分割表（factory / ota_0 / ota_1 / data / nvs）
└─ managed_components/  dependencies.lock   ← IDF 依賴生成物，不進 git
```

CMake/ninja 的中間檔不在 `firmware/`：`make.py` 用 `-B ../artifacts/build/<chip>_<board>`，
`-DSDKCONFIG` 也在同一個目錄。`firmware/sdkconfig` 已廢除。

`components/sys` 與 `components/tasks` 由根 `CMakeLists.txt` 的 `EXTRA_COMPONENT_DIRS` 掛進來。
新增一個分區（`hw/`、`sw/`…）時，要同時加進那個清單。

---

## 配置怎麼疊

```text
firmware/sdkconfig.defaults                  ← 共通（分割表 / FATFS / FreeRTOS / USB）
        ↓ 後者覆蓋前者
../ports/_profiles/<能力>.defaults           ← 能力碎片（ETH / WS / UDP），由 board.py sync 展開
        ↓
../ports/hardware/<chip>/<board>/sdkconfig.defaults   ← 硬體事實（flash / PSRAM / CPU / console）
        ↓ build 時展開
../artifacts/build/<chip>_<board>/sdkconfig   ← 生成物（每塊板一份，-DSDKCONFIG），不要手改
```

`firmware/sdkconfig` 已經廢除；`make.py` 用 `-DSDKCONFIG=` 指向 per-board build dir，
所以多塊板可以並存。舊的 `firmware/sdkconfig` 若還在，`make.py clean` 會清掉。

三個檔案各自的上位真相是 `../ports/hardware/<chip>/<board>/board.json`（機器可讀的板子身份證）。
project 層（`../ports/project/<chip>/<board>/`）**只有 runtime 設定**（`project.json` ＋
`data/config/*.json`），冇 `sdkconfig.defaults`，唔參與 build。

```bash
python3 ../tools/board.py check   # board.json ↔ sdkconfig 一致性 + 抓放錯層的設定
```

**規則：共通檔不准出現板子事實**（flash 大小 / PSRAM / console / CPU 頻率）——
`board.py check` 會擋。要調任何值 → 寫回 port 的 `sdkconfig.defaults` 或 `board.json`，
再 `python3 make.py build <board> --fresh`（只清這塊板的 build dir／sdkconfig）。

分割表同理：`partitions.csv` 放在 hardware 層，在 `sdkconfig.defaults` 用
`CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="../ports/hardware/<chip>/<board>/partitions.csv"` 指定
（相對本目錄）。

**hardware 預設是編出來的**：`tools/ncportgen.py` 把 `wiring.json` 變成
`nc_port.c`（static const `nc_hardware_cfg_t`）＋ `preset_hardware.json`；
firmware 不再在 runtime 解析 wiring JSON。

---

## 出廠預置怎麼進裝置

`main/CMakeLists.txt` 會把 `../ports/project/<board>/data/config/*.json` 用 `EMBED_FILES` 嵌進 firmware，
開機時寫進 `data` 分區。**不走** `fatfs_create_partition_image`——MSC 用 wear levelling，
預先燒的 FAT 映像會被 WL 的 metadata 擠歪，導致碟顯示「沒有檔案系統」。

- 開發日常 → `idf.py app-flash`（只換 app，不動碟上的使用者資料）
- 出廠 / 要重置 → `idf.py flash`（連預置一起蓋回去）

---

## 現況 vs 目標架構

[docs/03_notes/component-layout.md](../docs/03_notes/component-layout.md) 定的終點是七個分區
（`hw` `sys` `sw` `action` `tasks` `ui` `schema`），依賴只能向下。

**目前只有兩個分區落地**：`sys/`（協議、記憶體、任務、通道）與 `app/`（OTA）。
指令 handler 與裝配邏輯仍在 `main/main.c`（約 2.2k 行），是後續拆分的主要來源。
`hw` / `sw` / `action` / `ui` / `schema` 尚未建立。
