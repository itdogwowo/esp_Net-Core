# ports/ — 板子層（設定，不是程式）

`firmware/components/` 分「這是什麼程式」；`ports/` 分「**這塊板子要什麼**」。

原則：**晶片 = 一個 build，板子 = 一份描述。**
同晶片的 N 塊板子共用同一份程式，差異全部收在這裡。

---

## 1. 三層：哪些能「換板不改程式」，哪些不能

你會在 ESP-IDF 撞到的牆，就是這三層沒有分開：

| 層 | 例子 | 換板要不要重新 build | 放哪 |
|---|---|---|---|
| **L1 硬體事實** | flash 大小 / 頻率 / 模式、PSRAM 有冇與模式速度、CPU 頻率、有冇 Ethernet、console | ✅ 要 | `board.json` + `sdkconfig.defaults` |
| **L2 能力碎片** | ETH、NC4 over WS、NC4 over UDP 這種「功能包」 | ✅ 要 | `board.json` 的 `profiles` → `ports/_profiles/` |
| **L3 執行期** | gpio、服務開關、像素數、網路位址 | ❌ 同一 binary | `data/config/*.json` |

> **PSRAM、flash 大小、flash 頻率永遠不可能用 config.json 換**——它們在 bootloader 與
> 記憶體初始化階段就決定了。寫進 `config.json` 只會被 `tools/board.py check` 擋下來。

---

## 2. 目錄

```text
ports/
├─ _profiles/                      能力碎片（可組合、可重用）
│  ├─ eth-rmii.defaults            EMAC + DMA 1600 + lwIP 視窗調校
│  ├─ nc4-ws.defaults              CONFIG_HTTPD_WS_SUPPORT
│  └─ nc4-udp.defaults             LWIP UDP recv mailbox
│
├─ S3/ESP32-S3-N16R8/
│  ├─ board.json                   16 MB flash / 8 MB Octal PSRAM / console JTAG
│  ├─ sdkconfig.defaults           L1 硬體事實（與 board.json 一致）
│  └─ data/config/                 出廠執行期預置（一層一個檔）
│     ├─ system.json
│     └─ network.json
│
└─ P4/ESP32-P4-ETH/
   ├─ board.json                   32 MB / HEX 32 MB / RMII + IP101 / UART0 console
   ├─ sdkconfig.defaults           L1 + 檔尾的 L2 碎片（由 sync 產生）
   └─ data/config/                 出廠執行期預置
      ├─ system.json
      └─ network.json
```

---

## 3. 日常操作

```bash
python3 tools/board.py list         # 所有板子與能力一覽
python3 tools/board.py check        # 驗 board.json ↔ sdkconfig，並擋下放錯層的設定
python3 tools/board.py sync         # 改了 _profiles/ 或 profiles 之後重跑
tools/board.sh build p4             # 建置（板子用目錄名 / 晶片名都可以）
tools/board.sh boards               # 同上，從 board.sh 看
```

新增一塊板子：

```bash
python3 tools/board.py new \
  --chip S3 --board ESP32-S3-Matrix \
  --flash 16 --flash-freq 80 --psram 8 --psram-mode octal --psram-speed 80 \
  --cpu 240 --console usb-serial-jtag --eth none --wifi --ws
```

它會建好 `board.json` / `sdkconfig.defaults` / `data/config/*.json` / `README.md`，
並把 `--ws` 這類能力對應的碎片同步進去。**不需要改任何 shell 腳本或 CMake。**

---

## 4. `board.json` 是什麼

機器可讀的板子身份證；`check` 用它對照真正的 Kconfig：

```jsonc
{
  "name": "ESP32-P4-ETH",
  "chip": "esp32p4",
  "flash": { "size_mb": 32, "freq_mhz": 40, "mode": "dio" },
  "psram": { "size_mb": 32, "mode": "hex", "speed_mhz": 200 },
  "cpu_mhz": 360,
  "console": "uart0",
  "profiles": ["eth-rmii", "nc4-ws", "nc4-udp"],
  "features": { "eth": "rmii-ip101", "wifi": false, "usb": false },
  "verified": "2026-11-02",
  "notes": "rev1.3 樣品；flash 只能 40 MHz、CPU 上限 360 MHz"
}
```

`check` 會抓的錯（都是實際會讓你燒壞一小時的錯）：

- `board.json` 說 16 MB，`sdkconfig.defaults` 設 8 MB
- `board.json` 說有 PSRAM，`sdkconfig` 沒開 `CONFIG_SPIRAM`
- `board.json` 說沒有 Ethernet，`sdkconfig` 卻開了 `CONFIG_ETH_ENABLED`
- 宣告了能力碎片但沒 `sync`
- 把 `flash_size` / `psram` 這種 build-time 的東西寫進 `data/config/*.json`
- 共通 `firmware/sdkconfig.defaults` 偷渡板子事實

還有一個會靜默咬人的：**`firmware/sdkconfig` 存在的時候，改 `sdkconfig.defaults` 不生效**。
`check` 會比對 mtime 提醒你，`tools/board.sh build` 則會自己重設。

---

## 5. 能力碎片的規矩

- 碎片只放「**同一個原因而存在的多行設定**」（ETH 一定要配 DMA + lwIP 視窗）。
- 碎片裡不要出現任何 L1 事實（型號、腳位、頻率）。
- 改碎片 → `board.py sync` → 所有宣告它的板子一起更新；牆上的數字只有一份。
- `sdkconfig.defaults` 檔尾那段是**產生出來的**，手改會在下次 `sync` 被蓋掉；
  這塊板子獨有的東西請寫在 marker 區塊上方。

---

## 6. 出廠預置的兩個坑

**① 不走 `fatfs_create_partition_image`。** MSC 用 wear levelling，WL 會把分區開頭拿去放自己的
metadata，預先燒在 offset 0 的 FAT 映像會對不上 → 碟顯示「沒有檔案系統」。
現在的做法：firmware 內嵌 `config.json` → 第一次開機自動格式化 → 寫入預置 → 交給 host。

**② `idf.py flash` 會蓋掉使用者改過的內容。** 所以：

- 開發 / 日常 → `app-flash`（只換 app，不動碟）
- 出廠 / 要重置 → `flash`（連預置一起蓋回去）

預置不是保證：使用者可以刪掉 `config.json`。**缺檔必須能開機**，不能因此進不了 RUN。

---

## 7. 跟 MicroPython 版（mp_Net-Core）的對照

MPY 的 `ports/<chip>/<board>/` 是**部署 delta**：`slave/` 是基礎，port 用「同路徑覆蓋」
放 `main.py`、`action/*.py`、`config.json`、`schedule.json`——因為 MicroPython 的程式
本身就是檔案系統上的檔案。

C 版**刻意不照抄**：

| MPY | C 版 | 為什麼 |
|---|---|---|
| `slave/` | `firmware/` | 一樣：乾淨基礎 |
| port 可放 `*.py` 覆蓋程式 | ❌ 只能放設定 | 程式要編譯；per-board code = per-board build |
| port 的 `config.json` 覆蓋 `slave/config.json` | `data/config/*.json` 是出廠預置，runtime 屬使用者 | 同一份 FAT，不疊層 |
| 15 塊板子並存 | 目前 2 塊，`board.py new` 隨時加 | 沒 driver 支撐的板子不要先開空殼 |

MPY 那邊 15 個 port 有一半靠的是「同一個 Python 基礎 + 不同 config」；C 版把這件事
做得更硬：不只要 config，還要 `board.json` 把**不可 runtime 的部分**釘死在 build 期。

---

## 8. 什麼不該放這裡

- 程式 → `firmware/`
- 給人看的說明 → `docs/`
- 主機端工具 → `tools/`

分層理由見 [docs/03_notes/component-layout.md](../docs/03_notes/component-layout.md) §4。
