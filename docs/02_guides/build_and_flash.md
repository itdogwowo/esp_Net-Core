# 建置與燒錄（ESP-IDF v6.1 / macOS 或 WSL Ubuntu）

> **用途**：從零把這台機器弄成「能 build esp_Net-Core」的狀態，並說明燒錄的三種模式。
> **對象**：第一次在這台機器 build 的人；或 IDF / Python 被升級後壞掉的人。
> **最後驗證**：2026-11-16，WSL Ubuntu + ESP-IDF v6.1，從乾淨的 `artifacts/build/` 重建通過
> （P4-ETH `0xff040` ≈ 1020 KB、S3-N16R8 `0x118e90` ≈ 1.1 MB）。

> ⚠️ **指令在哪一層跑**：ESP-IDF 工程根是 **`firmware/`**（`firmware/CMakeLists.txt`）。
> 下面的 `idf.py ...` 全部要在 `firmware/` 下執行；`tools/*` 則在 repo 根執行（它們會自己找路徑）。

---

## 0. 一句話

```bash
# 推薦：一支搞定（自動處理 target 切換、產物備份、環境健檢）
python3 make.py doctor              # IDF / esptool / 板子設定 / 磁碟
python3 make.py build ESP32-P4-ETH  # 硬體板名 / <chip>/<board> / project 名都可以
python3 make.py flash ESP32-P4-ETH

# 手動等價
. ~/esp/esp-idf/export.sh
cd firmware
idf.py -DPORT=P4/ESP32-P4-ETH -DIDF_TARGET=esp32p4 build
```

建置產物全部集中在 `artifacts/`：

```text
artifacts/
├─ <board>.bin                 最新 app image（OTA 用這個）
├─ <board>_<時間>.bin          舊的歷史檔
├─ <board>.json                尺寸 / sha256 / git / IDF 版本
├─ <board>/                    這顆的完整可燒 .bin 組
│  ├─ esp_net_core.bin
│  ├─ bootloader.bin
│  ├─ partition-table.bin
│  ├─ ota_data_initial.bin
│  ├─ flash_args
│  └─ manifest.json
├─ build/<chip>_<board>/       所有 CMake / ninja / .o 中間檔（每塊板一個）
└─ logs/<board>_<時間>.log
```

**`firmware/` 不再產生 `build/`、`build_p4/`、`build_p4c6/`、`sdkconfig`。**
每塊板的 `sdkconfig` 在 `artifacts/build/<chip>_<board>/sdkconfig`，所以多塊板可以
並存、不用每次切 target。`python3 make.py clean` 會清掉整個 `artifacts/build/`
以及舊的 `firmware/build*`／`sdkconfig*`。

前提是第 2 節裝好了、第 3 節的地雷修過了。

---

## 1. 環境（2026-10-31 實測）

| 項目 | 值 |
|---|---|
| OS | macOS 26.2（build 25C56），Apple Silicon |
| ESP-IDF | **v6.1**（WSL 的 `/home/user/git/s31-verify/esp-idf`；macOS 的 `~/esp/esp-idf` 仍有 v6.0.3 可退回） |
| 工具鏈 | `xtensa-esp-elf-gcc` 15.2.0（esp-15.2.0_20251204） |
| Python | 3.12.14（Homebrew），venv 在 `~/.espressif/python_env/idf6.0_py3.12_env` |
| esptool | 5.6.0 |
| managed components | `espressif/esp_tinyusb` 3.0.0（require idf >=5.3）、`espressif/cjson` 1.7.19~2 |

---

## 2. 安裝 IDF（只做一次）

```bash
# 1) clone（shallow + shallow submodules，約 350 MB）
mkdir -p ~/esp && cd ~/esp
git clone --depth 1 --branch v6.1 \
    --recurse-submodules --shallow-submodules \
    https://github.com/espressif/esp-idf.git esp-idf

# 2) 安裝 esp32s3 工具鏈（約 500 MB）
cd ~/esp/esp-idf
./install.sh esp32s3

# 3) 每次開新 shell 都要 source
. ~/esp/esp-idf/export.sh
idf.py --version        # 應印 ESP-IDF v6.1
```

> 為什麼裝在 `~/esp/esp-idf`：`tools/flash.sh` 和 `docs/02_guides/testing.md` 都假設這個路徑。
> 機器上另有 `mp_Make-Tools/lib/esp-idf`（**v5.5.5**），那是 MicroPython 版在用的，**不要動它**。

---

## 3. macOS 26 的兩個地雷（會讓 `install.sh` 失敗）

macOS 26 太新，Homebrew Python 3.12 的瓶裝有兩個問題。症狀不同，但**同一個根因**。

### 3.1 根因：`pyexpat` 鏈到系統 libexpat（缺符號）

```text
ImportError: dlopen(.../pyexpat.cpython-312-darwin.so):
  Symbol not found: _XML_SetAllocTrackerActivationThreshold
  Expected in: /usr/lib/libexpat.1.dylib
```

- Homebrew Python 3.12 是對 **expat 2.8.3** 編的，但 `pyexpat.so` 記錄的 install name 是 **系統的** `/usr/lib/libexpat.1.dylib`（較舊、沒有這個符號）。
- 連帶影響：`plistlib` 依賴 pyexpat → `platform.mac_ver()` 回傳**空字串**。
  這個副作用很陰險，因為它不會報錯，只會讓後面的工具拿到空版本號。

**修法**（把鏈接改到 Homebrew 的 expat，並重簽）：

```bash
SO=/opt/homebrew/Cellar/python@3.12/3.12.14/Frameworks/Python.framework/Versions/3.12/lib/python3.12/lib-dynload/pyexpat.cpython-312-darwin.so

cp "$SO" /tmp/pyexpat.so.bak    # 備份（之後 brew 升級會覆蓋，到時要重做）
install_name_tool -change \
    /usr/lib/libexpat.1.dylib \
    /opt/homebrew/opt/expat/lib/libexpat.1.dylib "$SO"
codesign -f -s - "$SO"

# 驗證：這兩個都必須正常
python3 -c "import pyexpat, platform; print(pyexpat.EXPAT_VERSION, platform.mac_ver())"
# 期望：expat_2.8.3 ('26.2', ('', '', ''), 'arm64')
```

> ⚠️ **`brew upgrade python@3.12` 會把這個修補蓋掉。** 升級後如果 `install.sh` / `cmark` / 任何 XML 相關工具又壞，重跑上面這三行。

### 3.2 症狀：pip 26 的 truststore 在 import 時就炸

```text
ValueError: invalid literal for int() with base 10: ''
  (pip/_vendor/truststore/_macos.py, line 22)
```

- 原因是 3.1 的副作用：`platform.mac_ver()` 是空字串，`"".split(".")` 得到 `['']`，`int('')` 失敗。
- **修好 3.1 之後這個會自癒**（我們實測：修完再跑 `install.sh` 直接成功）。

如果 3.1 暫時不能修，退化方案是手動鋪一個沒有 truststore 的 pip：

```bash
ENV=~/.espressif/python_env/idf6.0_py3.12_env
rm -rf "$ENV"
/opt/homebrew/opt/python@3.12/bin/python3.12 -m venv --without-pip "$ENV"
curl -sSLo /tmp/pip2431.whl \
  https://files.pythonhosted.org/packages/ef/7d/500c9ad20238fcfcb4cb9243eede163594d7020ce87bd9610c9e02771876/pip-24.3.1-py3-none-any.whl
"$ENV/bin/python" -m zipfile -e /tmp/pip2431.whl "$ENV/lib/python3.12/site-packages/"
./install.sh esp32s3
```

---

## 4. 第一次建置這個專案

```bash
. ~/esp/esp-idf/export.sh
cd <repo>/firmware            # ← ESP-IDF 工程根（沒有 firmware/ 這層的 CMakeLists 就找不到專案）

idf.py -DPORT=S3/ESP32-S3-N16R8 -DIDF_TARGET=esp32s3 set-target esp32s3   # 產生 sdkconfig、抓 managed_components（只需一次）
idf.py -DPORT=S3/ESP32-S3-N16R8 -DIDF_TARGET=esp32s3 build
```

`PORT` 相對於 repo 根的 `ports/`（CMakeLists 用 `../ports/${PORT}` 找）。

成功的樣子（2026-11-16，WSL + IDF 6.1）：

```text
P4-ETH  esp_net_core.bin binary size 0xff040 bytes. Smallest app partition is 0x400000 bytes.
S3-N16R8 esp_net_core.bin binary size 0x118e90 bytes. Smallest app partition is 0x400000 bytes.
```

`artifacts/`（`build/`、`<board>.bin`、`<board>/`、`logs/`）、`managed_components/`、
`dependencies.lock` 都在 `.gitignore` 裡，**不會污染 repo**。要重建乾淨環境就是
`python3 make.py clean && python3 make.py build <board>`；`firmware/` 不再生成
`build/` 或 `sdkconfig`。

> ⚠️ 改 `sdkconfig.defaults` **不會**套用到已存在的 per-board `sdkconfig`。
> 例如 `CONFIG_TINYUSB_CDC_RX_BUFSIZE` 改了卻沒生效，就是這個原因 —— 必須
> `python3 make.py build <board> --fresh`（只清這塊板的 build dir）再 build。

### 兩塊板子共用一棵樹：`tools/board.sh`

```bash
tools/board.sh build ESP32-S3-N16R8   # 硬體板 + project 由 ports/ 兩層決定
tools/board.sh flash ESP32-P4-ETH
tools/board.sh list                   # 偵測現在插的是哪塊
```

**踩過的坑（2026-11-02 已修）**：`-DPORT` 只換板子目錄，**不會換晶片**——
晶片來自 `sdkconfig` 裡的 `CONFIG_IDF_TARGET`。所以在 P4 之後跑 `board.sh build s3`，
其實是用 S3 的設定去建一份 **P4** image（大小、目標全錯），而且 S3 的程式碼根本沒被編到。
（同一時間還發現：`main.c` 的 TCP bus 區塊沒有用 `#if CONFIG_IDF_TARGET_ESP32P4` 包起來，
所以 **S3 從那個區塊進來之後就一直編不過**，只是沒有人真的用 S3 target 建置過。）

每塊板都有自己的 `artifacts/build/<chip>_<board>/` 與 `sdkconfig`，所以切板不再需要
`set-target` 重建整棵樹；所有可調值都必須寫在 `sdkconfig.defaults` /
`ports/hardware/<board>/sdkconfig.defaults`，不能只留在 menuconfig 的 sdkconfig 裡。

> 驗證方式（兩邊都要能編）：
> ```bash
> tools/board.sh build ESP32-S3-N16R8   # 期望 IDF_TARGET=esp32s3、image 0x118e90（≈1.1 MB）
> tools/board.sh build ESP32-P4-ETH     # 期望 IDF_TARGET=esp32p4、image 0xff040（≈1020 KB，含 WS/UDP/HTTP）
> ```

---

## 5. 燒錄：三種模式

### 5.0 先看這張表（免得再問「這塊板怎麼燒」）

| 板子 | 平時更新 | 為什麼 |
|---|---|---|
| **S3-N16R8** | `tools/ota.py --port COM8 <bin>`（NC4 OTA 0x22xx） | USB 平時係 **TinyUSB CDC**，esptool 直接連會被當成資料埠。要 ROM 燒錄就對 CDC 送字串 `DFU`（或按住 BOOT 5 秒）令它重新枚舉成 download mode |
| **P4-C6** | `tools/ota.py`（有網）／`esptool -p COM21`（USB-Serial-JTAG） | P4 冇 USB device（`board.json usb:false`），但 USB-Serial/JTAG **一直都在**，esptool 直接可用；console 是 UART0 主 ＋ USB-JTAG 次 |

> **S3 不是「只能 CDC、燒不了」**：OTA 不需要 download mode，`DFU` 字串也不需要拆線。
> 這一點以前漏記，導致重複問。

| 指令 | 燒什麼 | 什麼時候用 |
|---|---|---|
| `idf.py app-flash` | 只燒 app | **日常**。不動 NVS、不動 data 分區（碟上的檔案會保留） |
| `idf.py flash` | bootloader + 分割表 + app | 換分割表、換 flash 大小、出廠 |
| `idf.py erase-flash` + `flash` | 全清再燒 | 碟要回到出廠預置、或 NVS 壞掉 |

本專案用的是 `tools/flash.sh`，它多做了兩件事：

1. **自己找 ROM 埠**（app 跑著 TinyUSB 時，esptool 進不了 download mode）
2. 燒完用 `watchdog-reset` 把板子踢回 app，並印心跳

```bash
tools/flash.sh              # app-flash
tools/flash.sh --full       # bootloader + 分割表 + app
tools/flash.sh --erase      # 全清
```

沒有板子時的逃生門（見 `main.c` 註解）：

- 按住 **BOOT 不放約 5 秒**（3 秒觸發重啟，手不要放開，ROM 才看得到 GPIO0=0）
- 或對 app 的 CDC 埠送字串 `DFU`

之後更新 firmware 不需要 download mode，直接用 OTA：

```bash
tools/ota.py --port /dev/cu.usbmodem* artifacts/ESP32-P4-ETH.bin
tools/ota.py --port /dev/cu.usbmodem* --abort      # 清掉中斷／超時留下的 OTA session
```

> **BEGIN 等幾秒是正常的**：韌體的 `OTA_BEGIN` 會先把目標分區整段抹掉才回 ACK
> （S3 實測 ≈237 KB/s → 1.2 MB ≈5.2 s）。工具會照映像大小等（`10 s + size/100 KB`），
> 唔會再硬編 5 s。
> 如果上一次 OTA 中斷，裝置會停在 `state=writing`；先 `--abort`，否則下一次 BEGIN
> 會被拒（`err_begin_partition_conflict`）。詳見
> [protocol commands §2 OTA 時間語義](../01_protocol/commands.md)。

---

## 6. 疑難排解

| 症狀 | 原因 | 處理 |
|---|---|---|
| `install.sh` 在 ensurepip / venv 失敗 | 3.1 的 libexpat | 套用 3.1 的修法 |
| `pip` 報 truststore ValueError | 同上 | 同上 |
| `idf.py` 找不到 | 沒 source export.sh | `. ~/esp/esp-idf/export.sh` |
| 改了 `sdkconfig.defaults` 沒生效 | sdkconfig 在 build dir 裡（`artifacts/build/<chip>_<board>/sdkconfig`），已存在的不會被覆蓋 | `python3 make.py build <board> --fresh`（刪掉那份 sdkconfig 再建） |
| `idf.py flash` 找不到埠 | app 佔著 TinyUSB | 用 `tools/flash.sh`（會等 ROM 埠） |
| 燒完板子沒反應 | hard-reset 對 USB-JTAG 無效 | `tools/flash.sh` 已用 watchdog-reset 處理 |
| `esp_tinyusb` 抓不到 | component manager 沒網路 | 檢查 `dependencies.lock` 與網路 |

---

## 7. 相關

- `docs/02_guides/testing.md` §0 環境表（本文件是它的展開）
- `tools/flash.sh` / `tools/ota.py` / `tools/nc4.py` / `tools/selftest.py`
- `firmware/sdkconfig.defaults`（晶片層）與 `ports/hardware/<chip>/<board>/sdkconfig.defaults`（板子層，後者覆蓋前者）
