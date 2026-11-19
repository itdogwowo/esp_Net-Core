# 測試編譯層：由「一個開關」改成「測試版本號」

> **狀態：已被 [test-ids.md](test-ids.md) 取代（2026-11-19）。** 本頁保留作歷史：
> 分級（level 1–4）已改成「測試 ID」（0 = 公共、1 measure、2 network、3 pixel、4 ncm）。 這頁只定契約；改程式與 `make.py` 是下一步。
> 相關：[execution-rules.md §6](execution-rules.md)、[pixel-renderer.md §13](pixel-renderer.md)、
> [configuration.md §6](../02_guides/configuration.md)

---

## 1. 問題：一個 boolean 控制 58 條指令 ＋ 17 個區塊，測試互相污染

現況只有 `NC_ENABLE_TESTS`（ON/OFF）：

- `test_action` 有 **58 條 `0xF0xx`** 指令，全部一起進／一起出；
- `main.c` 有 **17 個 `#ifdef NC_ENABLE_TESTS`** 區塊；
- 開機有 **2 個測試 hook**：`nc_test_action_espnow_boot()`、`nc_test_action_netauto_boot()`；
- tests 會**強制打開 NCM**（WAMR＋512 KB arena）。

結果是「要量 A，B 一定在旁邊」：

| 實際踩到的污染 | 後果 |
|---|---|
| `espnow_boot` 在測試映像開機就 `esp_wifi_init`＋ESP-NOW | 記憶體量測多 56 KB、多 `wifi`／`tcpip`／`espnow_rx` 任務（§7.124 更正） |
| `0xF062` 16×600 第二次執行 | 直接重置板子（bench 內部狀態／記憶體） |
| `0xF050` 兩條指令同號（`CPU_BURN` 與 `TCP_SEND`） | 後者被丟棄，測試「靜靜地唔見了」 |
| tests 自動開 NCM | 每張測試映像都背 512 KB arena，量出來的 heap 不是產品的 heap |
| `iface=wifi` 的測試線 | 在有編 WiFi 的映像才會 bind；換 build 行為就變 |

一句話：**以「有沒有測試」分類太粗，應該以「這一版測試要碰什麼」分類。**

---

## 2. 設計：**開關 ＋ 版本號**（兩個旋鈕，唔係二選一）

保留 `NC_ENABLE_TESTS`（ON/OFF）做**總開關**，另加 `NC_TEST_LEVEL`（1–4）做**版本號**：

| 旋鈕 | 回答什麼 | 取值 |
|---|---|---|
| `NC_ENABLE_TESTS` | 這顆映像**是不是**測試映像 | OFF／ON |
| `NC_TEST_LEVEL` | 若是，測到**哪一層** | 1 measure／2 network／3 pixel／4 ncm |

- **OFF** → 版本號無意義（C 裡 `#ifdef NC_ENABLE_TESTS` 外層就擋掉），映像 0 B 測試。
- **ON** → 內層再用 `#if NC_TEST_LEVEL >= N` 收那一層。
- 為甚麼唔用「level=0 代表 release」：`0` 與 `OFF` 會變成同一件事的兩種寫法，
  工具與側錄就要猜哪個才是真相。**開關是唯一真相，版本號只是它的細分。**

每一級是前一級的超集，級別按「會不會污染」排：

| Level | 名稱 | 內容 | 污染面 |
|---:|---|---|---|
| **0** | release | 冇任何 test 組件 | — |
| **1** | `measure` | 純量測指令：`0xF001` DEV_BENCH、`0xF003` HUB_BENCH、`0xF01C` MEM_BENCH、`0xF030` BUF_TEST、`0xF050` CPU_BURN…（唔碰網路、唔碰燈） | 只佔 static＋stack |
| **2** | `network` | ＋`0xF02x` Wi-Fi/ESP-NOW 指令、`0xF04x` net bench、rawbench 線、`espnow_boot`／`netauto_boot` hook | **會初始化 WiFi／netif** |
| **3** | `pixel` | ＋`0xF060`–`0xF062`（producer／render／PXBENCH） | **會起 pixel 後端、claim I80 腳** |
| **4** | `ncm` | ＋`NC_ENABLE_NCM`（loader／WAMR／arena 512 KB） | **PSRAM 512 KB＋WAMR** |

規則（五條）：

1. **開關是外層**：`if(NC_ENABLE_TESTS)` 才收 test 組件；C 裡 `#ifdef NC_ENABLE_TESTS`
   是最外層，內層才 `#if NC_TEST_LEVEL >= N`。OFF 時連版本號都唔用。
2. **累加、不亂序**：L3 一定包含 L1／L2。要「只有 pixel 冇 network」就係新增一級，
   唔准用「L2 唔要、L3 要」的組合。
3. **開機 hook 係獨立的閘**：`espnow_boot`／`netauto_boot` 除了 level，**仲要**由
   runtime config（`network.json`）允許才跑 —— 建置層唔應該替使用者決定「開機就連」。
4. **側錄分兩個欄位**：`"tests": true/false`（開關）＋ `"test_level": N`（版本號）。
   release 的 `"tests": false`，`test_level` 可以省或寫 0；唔好再用 `tests: true`
   假裝有版本資訊。
5. **工具一次傳兩個**：`-DNC_ENABLE_TESTS=ON -DNC_TEST_LEVEL=3`；唔好靠 CMake cache
   記住上一個值（2026-11-08 踩過）。

### 2.1 為什麼是「版本號」而不是 bitmask

bitmask（`TEST_NET | TEST_PIXEL`）更自由，但組合爆炸、每加一條要驗 N 種映像；
版本號是**線性**的，任何一塊板只會建「連續幾級」，測試矩陣可控。
真需要「跳級」時再加一新級，而不是開放任意組合。

---

## 3. 對照：現有的 17 個區塊怎麼分

| `main.c` 的區塊（約） | 位置 | 新 level | 備註 |
|---|---|---|---|
| SD bench 緩衝（`sd_bench_buf_*`） | 760–793 | **L1** | 只在 `0xF012` 用 |
| core action 的測試表（`DEV_BENCH`…） | 1822／2004／2202／2625 | **L1** | 量測指令 |
| `nc_test_action` include／hooks | 2625／3336–3381 | **L1** | 表本身（**清單已刪**：component 自己登記，見 commands.md §1.1.1） |
| 測試 task 註冊（bench/tcp，已刪） | 3338 | — | 已移除 |
| `espnow_boot`／`netauto_boot` | 4379–4390 | **L2**（＋runtime 閘） | 唯一會自動開 radio 的 |
| ncm 引擎／模組載入 | 4396–4426 | **L4** | 現時 `NC_ENABLE_TESTS` 自動開 |
| PXBENCH 指令（0xF062） | main.c 1286／3371（`CONFIG_NETCORE_PXBENCH`） | **L3** | 已經是獨立 Kconfig，可保留 |

> `test_action` 內的 58 條指令按 domain 再分一次：`on_*` 前面的註解已經有
> `0xF0xx` 域，分 L1／L2／L3 只需把「會碰 network」與「會碰 pixel」的搬去對應段落，
> 用 `#if NC_TEST_LEVEL >= N` 包住。

---

## 4. 工具層（下一步，唔喺今次）

> **現況（2026-11-19）**：CMake 骨架已接好 —— `NC_TEST_LEVEL`（1..4，預設 1）
> 已加、已傳去 `main` 與 `test_action`（`-DNC_TEST_LEVEL=1` 建置驗證 OK）。
> **C 碼仲未按 level 分支**（17 個區塊、58 條指令仍是全有全無）；工具亦未改。

- `make.py build <board> --tests` → `--tests[=N]`（無值＝L1，保持兼容）；
  傳 `-DNC_TEST_LEVEL=N` 而不是 `-DNC_ENABLE_TESTS=ON`。
- `--tests=0` 等於唔寫（release）。
- 側錄 JSON 寫 `"tests": N`；`tools/README.md` 與 `--help` 同步。
- **每次都要明確傳**（同今日 `NC_ENABLE_TESTS=ON/OFF` 的理由一樣：CMake cache 會記住舊值）。

---

## 5. 驗收（做完之後要證明）

| 檢查 | 方法 |
|---|---|
| L1 映像冇 Wi-Fi／netif 任務 | `status rt` 冇 `wifi`／`tcpip`／`espnow_rx`；`status sys` 的 `h_int` 與 release 差 < 30 KB |
| L2 映像才有 `espnow_boot` | 開機 log 的「ESP-NOW boot init」只出現在 L2+ |
| L3 映像才有 `i80` lane | `0xF061` 在 L1／L2 回 `err`（指令不存在），L3 才有 lane |
| L4 映像才有 arena | `status mem` 的 `ncm.arena` 只在 L4 |
| 測試之間唔再污染 | L1 連跑 `0xF001`／`0xF01C`／`0xF050` 唔會重置；L3 的 `0xF062` 唔會影響 L1 的數字 |
