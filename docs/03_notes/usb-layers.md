# USB 配置地圖：晶片 → 板子 → 用戶

> 2026-11-16。目的：回答「USB 到底在哪裏配置？」「有幾多條通道？」「邊個決定用不用、用哪個模式？」
>
> 一句話：**晶片決定有幾多條 PHY；板子決定邊條接出來、console 用哪個；用戶 config 決定
> personality（USJ／OTG）同 function（NC4／log／MSC）。**

---

## 1. 五個地方，各管一件事

| 層 | 檔案 | 決定什麼 | 例子 |
|---|---|---|---|
| **晶片能力** | `ports/hardware/_chips/<chip>.json` | 有冇 OTG／USJ、HS／FS、核數 | `esp32s3`：OTG＋USJ，共用 PHY |
| **板子事實** | `hardware/<chip>/<board>/board.json` | 這塊板有冇接 USB、console 是哪個 | P4-ETH：USB-C 冇接 OTG；S3-Matrix：USJ |
| **Build 開關** | `hardware/<chip>/<board>/sdkconfig.defaults` | 哪個 driver 編入、OTG FS/HS、console | `CONFIG_NETCORE_USB_OTG`、`CONFIG_ESP_CONSOLE_*` |
| **硬體註冊（新）** | `project/<chip>/<board>/data/config/hardware.json`（裝置 `/data/config/hardware.json`） | 有幾條 USB port、PHY、支援的 personality | `USB.list[]` |
| **用戶 runtime（interim）** | `project/<chip>/<board>/data/config/system.json` | 用哪條 link、哪個 personality、要什麼 function | `links[].kind`、`usb.msc`、`usb.cdc_log` |
| **未讀的舊層** | `hardware/<chip>/<board>/driver.json` 的 `USB` | 舊設計的 build-time device 段 | firmware **未讀**，不要以它為準 |

> `board.json` 的 `features.usb` 目前也**未被 code 讀**；真正開關是
> `CONFIG_NETCORE_USB_OTG`（build）＋ `system.json`（runtime）。

---

## 2. 每顆晶片有幾多條 USB 通道

| 晶片 | PHY／通道 | 可同時用 | 出廠 link | 備註 |
|---|---|---|---|---|
| ESP32 | 0 | 0 | — | 冇 USB |
| ESP32-S2 | 1 × OTG FS | 1 | `cdc0` | 冇 USJ |
| **ESP32-S3** | **1 條共用 PHY**（OTG FS 或 USJ） | **1（二選一）** | `usb_jtag0` | **MSC 一定要 OTG；不能用 USJ 出碟** |
| ESP32-C3／C5／C6／C61／H2 | 1 × USJ | 1 | `usb_jtag0` | 冇 OTG，冇 MSC |
| **ESP32-P4** | **3 條 PHY**：USJ FS、OTG FS、OTG HS | 硬件可 2+；**firmware 目前只開 1 條 USB link** | `usb_jtag0` | 多線並存未實作 |

「一條 USB link」= `system.json` 的 `links[]` 裡面一條 `kind:"usb"`（載入時解析成
`cdc` 或 `usb_jtag`），或者直接寫舊式 `kind:"cdc"`／`kind:"usb_jtag"`。
`usb_nc4_link()` 只取**第一條 enable 的 USB link**；多過一條會 warning，第二條不會建立。

---

## 2.5 硬體註冊：`hardware.json` 的 `USB` 段

2026-11-17 起，每塊板的 `ports/project/<chip>/<board>/data/config/hardware.json`
都有一份具體的 `USB` 段（generated snapshot；裝置 `/data/config/hardware.json` 同樣看得到）。
它**只註冊 port 事實**，不配 function、不指定誰用；GPIO 只有 read-only 的 `dm`／`dp`（定位用，不可調）：

```jsonc
// S3（只有一條共用 FS PHY；「OTG／USJ 二選一」由 _comment 提醒，不是 phy 值）
"USB": {
  "_comment": "S3：USB D-／D+ = GPIO19／20；只有一條 FS PHY，硬件在 phy:\"usj-fs\" 或 \"otg-fs\" 二選一（同一對腳）。GPIO 是固定 PHY 腳，唯讀，不可 remap。",
  "enable": 1,
  "list": [
    { "phy": "otg-fs", "enable": 1,
      "GPIO": { "dm": 19, "dp": 20 } }
  ]
}
```

| 欄位 | 意思 |
|---|---|
| `phy` | 實際 personality／PHY：`usj-fs`、`otg-fs`、`otg-hs`；S3 在 `usj-fs`／`otg-fs` 之間揀一個，**不要寫 `shared` 或 `fs`** |
| `GPIO` | **read-only**：這條 PHY 的 D-／D+（`dm`／`dp`）實際 GPIO；只作定位，不可改、不可 remap |
| `enable` | 這條 PHY 有冇接出來／用不用 |

**S3 怎樣二選一**：S3 只有一條 FS PHY，`wiring.json`（build 時的硬件事實）
的 `USB.list` 直接寫 `phy:"usj-fs"` **或** `phy:"otg-fs"`；同一對 GPIO19/20，
只能揀一個。轉另一個人格就是改 `wiring.json` 的 `phy` 再 **rebuild**
（`nc_port_usb_kind()` 由 build-time 生成），`system.json` 的 link 唔使改 ——
它寫的 `{kind:"usb",index:0}` 會跟住解析到新 personality。
**不需要在 link 再寫一次 `mode`**；`phy` 已經係硬件選擇。

> ⚠️ 裝置上的 `/data/config/hardware.json`（同 project 內那份 generated snapshot）
> 目前**只是鏡子**：它記錄 `USB.list`，但 firmware **未 parse** USB 段，
> 所以直接改它**不會**換 personality（ETH 段就有 parse）。要換就改 `wiring.json` 再 build。
`shared`／`fs` 都不是 PHY 值，唔再用。

| 板 | `USB.list` | USB GPIO（提醒用） |
|---|---|---|
| S3 Matrix／Mini（OTG） | `[{ "phy":"otg-fs", "GPIO":{"dm":19,"dp":20}, "enable":1 }]` | D-／D+ = GPIO19／20 |
| S3 N16R8／SuperMini（USJ） | `[{ "phy":"usj-fs", "GPIO":{"dm":19,"dp":20}, "enable":1 }]` | D-／D+ = GPIO19／20 |
| C3 | `[{ "phy":"usj-fs", "GPIO":{"dm":18,"dp":19}, "enable":1 }]` | GPIO18／19 |
| P4-ETH | `enable:0`、空 list | 板 USB-C 冇接 PHY；USJ 24/25、OTG FS 26/27、HS 49/50 都未接出 |
| P4-FireBeetle2 | `[{ "phy":"usj-fs", "GPIO":{"dm":24,"dp":25}, "enable":1 }, { "phy":"otg-hs", "GPIO":{"dm":49,"dp":50}, "enable":1 }]` | USJ 24／25；OTG HS 49／50 |

> 注意：`seed_file()` **只會建立、不覆蓋**。已經用過的裝置，`/data/config/hardware.json`
> 是舊版（冇 `USB` 段）；要見到新段就刪掉該檔重開機（會重新 seed），或手動加。
>
> **2026-11-17 起**：`system.json` 的 `kind:"usb"` 由 `nc_cfg_load.c` 解析 —— 用
> `index`（或舊式 `"usb":N`）查 build-time generated `nc_port_usb_kind()`
> （來源 `wiring.json` 的 `USB.list[].phy`），得出真 kind（`cdc`／`usb_jtag`）。
> 這塊板冇那條 port（生成表回 `0xFF`）→ 該 link **不建立**並計 `invalid`，
> 唔會靜靜當成 CDC。`USB.list` 的 `GPIO` 仍然只是 metadata（未 parse）；
> 完整 `sys/usb` claim 未實作，但 USB link 的 kind／name／msc 已經可以直接用。

### 2.6 USB 有幾多個可調 GPIO？

**0 個可調。** USB 的 D+／D-（S3 GPIO19/20；P4 USJ 24/25、OTG FS 26/27、HS 49/50；
C3 18/19）是 PHY 固定腳，不經 GPIO matrix，不能 remap；VBUS 亦不是用 GPIO 控制。
所以 `USB.list` 的 `GPIO.dm`／`GPIO.dp` 只是**唯讀定位資訊**（hardware.json 會記錄），
不是可改設定；將來亦不需要加其他 GPIO。

其他接口的可調 GPIO 另計：SPI（SCK／MOSI／MISO）、UART（TX／RX）、PWM、PIN、I2C、
I2S、I80 等可以經 GPIO matrix 選腳；ETH RMII 的 mdc／mdio／ref_clk 視 SoC EMAC
的 IOMUX 限制，不是全部自由。這些屬 Hardware 各自的 `GPIO` 段，不在 USB。

## 3. 用戶實際會改的三個開關

### ① `links[]`：指向 `USB.list` + MSC

實際 schema（2026-11-17 已落地）：

```jsonc
{ "name": "usb0", "kind": "usb", "index": 0, "msc": 1 }
```

| 欄位 | 意思 |
|---|---|
| `kind:"usb"` | 硬件類別（同 `kind:"uart"`／`"spi"` 一樣） |
| `index` | `USB.list` 的 index；也是線編號 |
| `msc` | 這條 USB 是否交碟（只對 `phy:"otg-*"` 有意義） |

- **不需要 `mode`**：personality 已經在 `wiring.json` 的 `phy` 揀了
  （S3：`usj-fs`／`otg-fs`；P4：`usj-fs`／`otg-fs`／`otg-hs`），build 時生成查表。
- **不需要 `usb` 欄位**：`index` 已經就是 `USB.list` index。
- 名用統一格式 **`usb<index>`**：`usb0`（`kind:"usb"` ＋ index）。
  personality 由 `phy` 決定，唔入名；routes 用**你寫的名字**（有寫 `name` 就用它）。
- name 優先序：非空 `name` 優先；冇寫或空字串 `""` → 用生成名。
- `name` 放在 object 最前面，同其他 link 統一。
- 舊的 `kind:"cdc"`／`"usb_jtag"`＋`"usb":0` 仍然接受作兼容，但新 seed 已全部遷移。

### ② `msc`：交不交碟

寫在 USB link 上（`"msc":1`），因為它是「這條 USB port 要不要交碟」；
`hardware.json` 不記 MSC。

- **預設 0**：唔寫就是唔交碟（`nc_cfg_load.c` 寫死 `l.msc = 0`，冇「未指定」狀態）。
  global `system.json` 的 `usb.msc` **已經停用**、唔會 fallback；舊檔寫了它，開機只會出一行
  warning 提醒改寫成 `links[].msc`。
- `msc=1`：B09 把 FAT 交給 host（只在 **OTG** 有意義；USJ 沒有這個 capability → 跳過／warning）。
- `msc=0`：`/data` 留在 APP；主機不會見到碟。
- MSC descriptor 是編譯期（`CONFIG_TINYUSB_MSC_ENABLED=y`）；`msc=0` 只代表不交碟，
  driver 仍然安裝。

### ③ `usb.cdc_log`：log 走不走 CDC

- OTG：`cdc_log=1` 開一條 CDC log 管道（與 NC4 共用 USB）。
- USJ：log 永遠開，與 NC4 共用 TX；`cdc_log` 無效。

### ④ `routes[]`：USB link 去哪裡

```json
{ "in": "usb0", "out": ["decode0"] }
```

現時 seed 用 `kind:"usb"` ＋ 顯式 `name:"usb0"`；舊檔寫 `cdc0`／`usb_jtag0` 的是
晶片表預設（**冇** project seed 時才會出現）。無 `name` 時生成名是 `usb<index>`。

---

## 4. S3 想開「隨身碟」（MSC）的完整做法

**先接受一件事**：S3 的 OTG 與 USJ 共用同一條 PHY，**OTG 開的時候 USJ console 一定消失**。
MSC 只有 OTG 有，所以這是一個 board-level 的取捨，不是單純加一個 runtime 開關。

### 4.1 `system.json`

```jsonc
{
  "usb": { "msc": 1, "cdc_log": 1 },
  "nc4": {
    "links": [
      { "kind": "cdc", "index": 0 },
      // …其他 WiFi／TCP／UDP lines
    ],
    "routes": [
      { "in": "nctx",  "out": ["decode0"] },
      { "in": "cdc0",  "out": ["decode0"] },
      // …其他 routes
    ]
  }
}
```

### 4.2 `sdkconfig.defaults`：console 二選一

| 選擇 | 設定 | 適合 |
|---|---|---|
| **UART0** | `CONFIG_ESP_CONSOLE_UART_DEFAULT=y` | 板子 UART0 有引出、你有 USB-UART 轉接 |
| **完全冇 console** | `CONFIG_ESP_CONSOLE_NONE=y` | 只有一條 USB；app log 由 CDC（B09 之後）出，bootloader 訊息冇 |

不可以保留 `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` 再開 OTG —— 同一條 PHY 會打架。

### 4.3 Build／flash

```bash
python3 make.py build <board> --fresh
python3 make.py flash <board> --full   # 分割表沒改；--full 只是確保格式一致
```

`--fresh` 必須：`sdkconfig` 有 cache，不 fresh 改了 `sdkconfig.defaults` 也不會生效。

### 4.4 主機看到什麼

- 一個 **MSC 碟**（卷標 `NC-xxxx`，容量 = `data` 分區，Matrix 4MB 板是 896 KB）。
- 一個 **CDC serial**（如果 `cdc_log=1`；app log）。
- `msc=1` 時 B09 已把 FAT 交 host；device 只剩唯讀 `/ro`，
  HTTP static `/data/www` 可能讀不到，測試時要預期。

### 4.5 本專案現況（2026-11-17 重驗）

- **ESP32-S3-Matrix 與 ESP32-S3-Mini-Nano 已配置成 OTG MSC**：link 寫
  `{kind:"usb",index:0,msc:1}`（wiring `otg-fs` → 載入時解析成 `cdc`）、
  `cdc_log=1`、`CONFIG_ESP_CONSOLE_NONE=y`；**build 已通過**（2026-11-17）。
  上板後**預期**主機見到 composite MSC＋CDC；app log 由 CDC 出 —— 未上板實測。
- **其他 S3（N16R8／SuperMini）**：wiring `usj-fs` → 解析成 `usb_jtag`、
  link 冇寫 `msc`（跟 global 預設 0），console 仍然是 USJ。
  要開 MSC 就跟上面步驟，先決定 UART0 還是 none。

---

## 5. 多 USB：kind、index、PHY 的分工

| 欄位 | 意思 | 不是 |
|---|---|---|
| `kind` | `usb` = 硬件類別（載入時由 `USB.list[index].phy` 解析成 `cdc`／`usb_jtag`）；舊式可直接寫 `cdc`／`usb_jtag` | 不是 PHY 編號 |
| `index` | USB link 指向的 `USB.list` index，同時是線編號 | 不是 PHY 編號 |
| `CONFIG_NETCORE_USB_OTG_HS` | P4 的 OTG 用 HS PHY（build-time） | 不是每條 link 的 runtime 選擇 |

- **今天**：每塊板只建立**第一條 enable 的 USB link**。寫兩條 `links[]` 只會 warning，
  第二條不會初始化（`usb_nc4_link()`）。
- **S3**：一條 shared PHY → 只能 `cdc` **xor** `usb_jtag`；`index` 沒有第二條可指。
- **P4**：硬件有三條 PHY，但 firmware 仍然只開一條 USB link。
  `CONFIG_NETCORE_USB_OTG_HS` 只決定 OTG 走 HS 還是 FS，不是「兩條 link 各指一條 PHY」。
- 所以「kind 會指向兩種」的答案：**不會**；kind 是 personality，一條 link 一個 kind。

### 5.1 目標：資源註冊 + claim，不是 `functions` 配對

之前草稿寫過 `usb.ports[]`＋`functions`，**這個方向撤回**。它把「資源」和「消費者」
預先配對，和本專案的「註冊資源 → 誰要用誰 claim」相反。

目標模型：

```text
Build／板子：註冊 port（只講能力，不講誰用）
  usb.port0: phy=otg-fs（S3 揀 OTG）, supports={nc4,log,msc}
  usb.port1: phy=usj-fs（S3 揀 USJ）, supports={nc4,log}
  usb.port2: phy=otg-hs（P4 HS）, supports={nc4,log,msc}

Runtime：consumer 自己 claim
  nc4  → claim(NC4)    // 要一條 USB 資料通道
  log  → claim(LOG)    // 要一條 log 出口
  msc  → claim(MSC)    // 要 MSC；只有 OTG port 支援

Registry：找一個同時滿足所有 claim 的 port
  · 有 MSC claim → 該 port 的 personality 必須 OTG
  · 冇 MSC → 有 USJ 就用 USJ；否則 OTG
  · 同一 port 可以同時有 nc4＋log＋msc（OTG composite）
  · 找不到 → 該 consumer disabled，不令 boot 停
```

config 只需要「啟不啟用 consumer」（tasks／service 的 level／affinity），
**不需要**寫 `functions` 或把 port 配給某個 service。這樣：

- 用戶唔會見到 `usb.ports[]`／`functions`（根本不用寫）；
- 誰要用 USB 就自己 claim；衝突由 registry 判，claim 失敗只影響該 consumer；
- P4 多 PHY 時，registry 自動分配，不需要用戶手動配對。

> 未實作：現在仍是 `links[].kind`（`usb` 載入時解析成 `cdc`／`usb_jtag`）＋
> per-link `msc`（預設 0；global `usb.msc` 已停用）／global `usb.cdc_log`。
> 要做的是 `nc_usb_port_register()`／`nc_usb_claim()` 與 per-port 狀態。
> 在接 NCM 之前不動這一層。

### 5.2 這一層屬於哪裡？不是 router

一句話：**router 是 data plane；USB port registry 是 control plane。**

```text
board／chip（capabilities）
        │  build-time 生成
        ▼
sys/usb（register port → collect claims → allocate）
        │  回傳 handle（port／personality／functions 的 union）
        ▼
bus/usb（USJ 或 TinyUSB driver；真正 init PHY）
        │
        ▼
nc4 line（cdc0／usb_jtag0）──► nc4_router（line → decodeN／forward）──► decoder
        ▲
        │ claim(NC4 / LOG / MSC)
task_manager（consumer 生命週期；佢只帶 claim，不擁有 hardware table）
```

| 層 | 負責 | 不負責 |
|---|---|---|
| **`nc4_router`** | 一條已存在的 line 的幀去哪裡（decodeN／轉發） | PHY、personality、MSC、claim |
| **`sys/usb`（新）** | 註冊 port、收 claim、依 claims 推導 personality／active functions（不是預先配對） | 幀路由、任務排程 |
| **`bus/usb`** | USJ／TinyUSB driver、PHY init | 決定誰用 |
| **`task_manager`** | consumer 的生命週期、level／affinity | 硬體分配 |
| **`nc_hw`** | pin／bus claim、owner command／snapshot | USB port 的 composite 政策 |

所以 USB port registry **不屬於 router**。Router 只會見到「已經分配好的 line」；
把 PHY／personality 放進 router，等於叫 data plane 認識 hardware，會破壞分層。

**為什麼不放在 `nc_hw`**：`nc_hw` 現在是 pin claim ＋ owner command／snapshot；
USB 有「shared PHY、personality 二選一、composite functions」這些額外政策。
先獨立成 `sys/usb`，將來真的有第二種「多 personality 資源」再抽成通用 `sys/res`。

**Boot 次序（目標）**：B03 註冊 port＋收 claim（因為 B05 router 要用已分配的
USB line），B05 建 router，B06 任務拿 handle，B09 才 init driver。

## 6. 未做好（下一步）

1. **多 USB 通道**：P4 有三條 PHY，但 `usb_nc4_link()` 只取第一條；
   `usb.rx` 仍然是全域，`usb.cdc_log` 也是全域（`msc` 已有 per-link）。全部 per-port 未做。
2. **`hardware.json` 的 `USB` 段**：firmware **未 parse**（只是 generated snapshot／鏡子）；
   personality 只認 build-time 的 `wiring.json` → `nc_port_usb_kind()`。
   `driver.json` USB 段與 `board.json features.usb` 亦未讀。
3. **MSC descriptor** 是編譯期；`msc=0` 仍安裝 driver，未做到「唔要就完全唔編」。
4. **USJ 沒有 MSC**：不要用 `msc=1` 配 USJ 解析出的 link，那只會 warning。
