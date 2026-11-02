# 燈效的「後期載入 + 編譯執行」：路線研究與 ABI

> **用途**：回答一個問題 —— 有沒有辦法像 MicroPython 一樣「後期把程式丟進去就跑」，但語法像 Go 一樣簡單、跑起來像 C 一樣快，而且能應付矩陣與多塊緩衝？
> **對象**：要選型的人、要動 effect → pixel pipeline 那一格的人。
> **狀態**：研究完成、建議已定；**P0 spike 未做**（見 §9，這是唯一會推翻結論的地方）。
> **相關**：[architecture-flow.md](architecture-flow.md)（compute 這一格）· [buffer-manager.md](buffer-manager.md)（effect 的記憶體從哪來）· [memory.md](memory.md)（pool / arena）

---

## 0. 結論

1. **有，而且實際上只有一條**：WebAssembly（WAMR；第一版跑直譯，需要時再上 AOT —— 見 §4.4.4）。它同時滿足四件事 —— 後期載入、編譯執行、**記憶體有界**、**跨晶片**（S3 的 Xtensa 與 P4 的 RISC-V 吃同一份 `.wasm`）。
2. **但「快」不是你要的東西。** 676 px @ 30 fps 有 1,333 萬 cycles，每像素 19,700 cycles。連直譯器都綽綽有餘。真正難的是**記憶體契約**（矩陣、多塊緩衝、RMT 不能指 PSRAM），而那件事跟語言無關。
3. 所以順序是：**先定 ABI（§6）→ 做資料驅動的 IR（§4.2，吃掉 80% 效果、零工具鏈）→ wasm 當逃生艙**。
4. **不要自己發明語言。** Pixelblaze 證明小語言在這一行能賣，但那是 3–5k 行編譯器 + 除錯工具 + 生態全自己來。
5. 語言選 **TinyGo**（真的是 Go 子集）。但 ABI 是 wasm，所以 Rust / Zig / C 之後都能進來 —— 你綁的是格式，不是語言。

一句話：**格式選 wasm，語言選 TinyGo，記憶體走你已經定案的 declare→commit，framebuffer 住在 guest 的線性記憶體裡。** 燒錄只發生在「核」變了的時候；效果、參數、映射永遠只是一次檔案傳輸（§3）。

---

## 1. 先把問題拆成三個獨立的軸

「像 mpy 但更快」把三件事混在一起了。分開看：

| 軸 | MicroPython | C 固件 | **你要的格子** |
|---|---|---|---|
| **語法**（誰來寫） | Python | C | Go 系 |
| **載入時機**（什麼時候進去） | 執行期丟 `.py` / `.mpy` | 編譯期燒進去 | **執行期** |
| **執行方式**（怎麼跑） | bytecode 直譯 | 原生機器碼 | **編譯 / 近原生** |

你要的是右下角那一格。**這個格子裡目前只有三個東西**：

1. **WASM + AOT** —— 位元組碼格式 + 上傳時編成機器碼（§4.4）
2. **原生 ELF 熱載入** —— 直接載 `.so`（§4.5）
3. **自訂 bytecode + 自寫 VM** —— Pixelblaze 路線（§4.3）

其他選項（Lua、JS、Berry、PikaScript…）都在「執行方式 = 直譯」那一格，不管語法多像 Go，都答不了你的問題。

> 三個軸之外還有第四個，遲早會咬人：**記憶體隔離**。mpy 有 GC heap（時間不可預測）；C 沒有隔離（一個越界就整台重開）。你的 buffer-manager 文件已經在解決「佈局」，但沒有解決「效果寫壞了會怎樣」。這件事只有 wasm 給得起。

---

## 2. 先算預算：為什麼「高效」可能不是你的瓶頸

`ports/P4` 的 `config.json` 是 **676 px（26×26）、30 fps**。P4 是 400 MHz 雙核 RISC-V。

| 情境 | cycles / 幀 | 每像素預算 |
|---|---|---|
| P4 400 MHz · 676 px · 30 fps | **13.3 M** | **19,700** |
| P4 400 MHz · 676 px · 60 fps | 6.7 M | 9,860 |
| P4 400 MHz · 4,096 px（64×64）· 30 fps | 13.3 M | 3,250 |
| S3 240 MHz · 676 px · 30 fps | 8.0 M | 11,800 |
| S3 240 MHz · 4,096 px · 30 fps | 8.0 M | 1,950 |

再對照「一個像素要花多少」：

| 執行方式 | 每條運算的粗估 | 一個像素 30 條運算 |
|---|---|---|
| 原生 C（有 FPU、LUT 取代 sin） | 3–10 cycles | 100–300 cycles |
| WASM AOT | 約 1–2× 原生 | 150–600 cycles |
| WASM fast interp | 約 10–30 cycles / bytecode | 300–1,200 cycles |

**結論：26×26 用直譯都夠（用掉預算的 5%）。64×64 以上、或每像素多層 noise / blur，才需要 AOT。**

這張表改變了選型的重點：**不要為了「快」去買一套工具鏈；要為了「載入 + 記憶體契約 + 隔離」去買。** 快只是順便。

> ⚠️ 上表的 interp / AOT 倍率是量級估計，不是量測。P0 spike 的第 4 項就是把它換成 P4 上的真實數字。

---

## 3. 分離式固件：一次燒錄，之後只傳檔案

真正的需求不是「有沒有一個語言」，而是**什麼東西可以變成檔案、什麼東西必須留在固件裡**。切錯這條線，換什麼語言都救不回來。

### 3.1 切在哪裡

| 層 | 內容 | 怎麼更新 | 要重開機 |
|---|---|---|---|
| **核（kernel）** | bootloader、分割表、NC4、檔案服務、驅動、frame clock、buffer manager、**WAMR + fx host** | 燒錄 / 整包 OTA | 要 |
| **效果** | `.fxe`（wasm/aot + manifest） | **檔案** | **不要**，下一幀生效 |
| **參數 / 映射 / 調色盤** | `config.json`、palette、pixel mapping | **檔案** | 不要（`ASSET_RELOAD`） |
| **schema**（可選） | 指令表、狀態表 | **檔案**（若做成 JSON） | 不要 |
| **碰硬體的** | 中斷向量、DMA 描述子、PHY 校正、面板初始化時序 | — | 只能燒錄 |

**判準一句**：**只依賴 ABI、不依賴絕對位址與時序的東西，可以變成檔案；碰到硬體寄存器的不能。**

這條線剛好就是 wasm 沙箱替你畫的那條線 —— 你原本在 `architecture-flow.md` 寫的「effects 不可以直接碰硬體」，在這裡變成**編譯器保證**，不是自律。

### 3.2 三條「檔案」通道（都不是燒錄）

| 通道 | 什麼時候 | 速度 | 20 KB 效果要多久 |
|---|---|---|---|
| USB CDC（NC4 file service） | RUN | ~1 MB/s | ~20 ms |
| UART 921600（NC4） | RUN | ~90 KB/s | ~0.2 s |
| WiFi / Ethernet TCP（NC4） | RUN | P4 已調到 8 MB/s | 瞬間 |
| USB MSC | RECOVERY（停機） | 當隨身碟拖 | 拖進去 |

最後一列是重點：**你已經有一個隨身碟了** —— `data` 分區 7 MB，RECOVERY 時整個交給 host。效果檔就是那個隨身碟裡的一個檔案，跟 `config.json` 並排。

### 3.3 什麼時候才需要再燒一次

| 事件 | 要燒嗎 |
|---|---|
| 改一個效果 / 加一個新效果 | ❌ |
| 改參數、調色盤、pixel mapping | ❌ |
| 同時駐留多個效果、隨時切換 | ❌ |
| 加一條新指令（schema 做成 JSON 的話） | ❌ |
| 換驅動、換面板、改 frame clock | ✅ |
| 升級 WAMR、改 ABI | ✅（一年一次的事） |

**這才是「分離式固件」的判準** —— 不是「有沒有兩個分區」，而是**多久需要燒一次**。這個設計把它從「每次改效果」降到「改 ABI 或驅動」。

> 你已經有 `ota_0` / `ota_1`。那不是替代方案，是**互補**：OTA 換「核」（3 MB、要重開機），檔案換「效果」（20 KB、不重開機）。兩條路都要有。

### 3.4 效果包怎麼走

`architecture-flow.md` §3 已經有：`FILE_BEGIN → FILE_CHUNK → FILE_END → validate → atomic rename → ASSET_RELOAD`。**效果包就搭同一班車**：

```text
Host                          Device
 tools/fx.py build             NC4 file service
   tinygo → .wasm              FILE_BEGIN("fx/fire26.fxe", size, crc)
   wamrc  → .aot                 ...
   pack   → .fxe               FILE_END
        │                        validate: header / abi / 預算 / 記憶體宣告
        └──────────────────────> atomic swap → fx_load("fire26") → 下一幀生效
```

三個好處是現成的：**壞檔不會半寫入**（staging）、**可以回上一版**（`last_good`）、**不用重燒**（`ASSET_RELOAD` 換指標）。

---

## 4. 六條路線

| # | 路線 | 語法 | 載入 | 速度 | 隔離 | 你這邊的工作量 | 風險 |
|---|---|---|---|---|---|---|---|
| A | **參數**（現有 config.json） | JSON | 執行期 | — | 無 | 0 | 表達力不足 |
| B | **效果 IR / DAG** | JSON 節點圖 | 執行期 | 原生 | 無（但無程式碼） | 中（~800 行） | 複雜效果寫不出來 |
| C | **自訂 DSL + bytecode** | JS 風（Pixelblaze） | 執行期 | 直譯 | VM 有 | **大**（3–5k 行） | 只有你會寫 |
| D | **WASM + AOT** | **Go（TinyGo）** | 執行期 | 近原生 | **有（沙箱）** | 中（~600 行 + 工具） | 工具鏈 / IDF 版本 |
| E | **原生 ELF 熱載入** | C / Rust | 執行期 | 原生 | 無 | 小（元件現成） | 綁 IDF、雙 ISA、當機 |
| F | **MicroPython + viper** | Python | 執行期 | 直譯（快版） | 有（GC） | 小（你有 mp 版） | GC 停頓、記憶體不可宣告 |

### 4.1 A：參數（現有）

`config.json` 的 `Pixel` 段調 speed / hue / brightness。**這已經吃掉最常見的 50%。** 不要小看它 —— WLED 的多數「效果」本質上就是「一個 kernel + 幾個參數」。

### 4.2 B：效果 IR（資料驅動）

一個效果 = 一張節點圖，節點是原生 kernel：

```json
{"effect": "fire", "nodes": [
  {"op": "noise2d", "scale": 3, "speed": 1.4, "out": "n"},
  {"op": "palette", "in": "n", "name": "fire", "out": "rgb"},
  {"op": "blur",    "in": "rgb", "radius": 1, "out": "rgb2"},
  {"op": "mirror",  "in": "rgb2", "axis": "x"}
]}
```

- **優點**：零工具鏈、零當機風險（沒有使用者程式碼）、可以在網頁上編輯、幾 KB 就一個效果。
- **缺點**：節點要一個個寫；表達力有天花板。
- 經驗值：`noise / plasma / gradient / palette / blur / warp / mirror / mask / blend / scroll / sparkle / text` 這 12 個節點大概能覆蓋 8 成的矩陣效果。
- 成本 ≈ 每個節點 30–80 行的 C + 一個 200 行的圖求值器。

**這是投報率最高的一層，而且它跟 §6 的 ABI 不衝突** —— 第 3 層的 wasm 效果可以「內部」用同一套節點，也可以完全不甩它。

### 4.3 C：自訂 DSL（Pixelblaze 路線）

Pixelblaze（ElectroMage）是同一領域的商業產品：自製的小型 JS 風格語言、裝置端編譯成 bytecode、`beforeRender()` / `render(index)` 的模型，跑在 ESP32 上賣了很多年。**它證明「小語言 + bytecode」在這一行可行。**

但代價是：lexer / parser / 型別檢查 / codegen / VM / 錯誤訊息 / 除錯器，3,000–5,000 行，而且要養。你現在連 buffer manager 的 P0 都還沒開工 —— 這個順序不對。

> 只有一種情況值得做 C：**P0 spike 全部失敗**（WAMR 在 IDF v6 上編不起來、AOT 跑不動、且你拒絕 interp）。那時候 C 是 fallback，不是首選。

### 4.4 D：WASM（先直譯、需要時 AOT）（建議）

**已查證的事實**（2026-11）：

| 事實 | 來源 |
|---|---|
| Espressif 有官方 component `espressif/wasm-micro-runtime` 2.4.0，targets 含 **esp32p4** / s3 / c3 / c6 / c5 | component registry |
| 該 component 的 CMake 預設 **`WAMR_BUILD_AOT=1`、`WAMR_BUILD_FAST_INTERP=1`** | `espressif/wasm-micro-runtime` 的 CMakeLists |
| WAMR 官方 README 的 footprint（cortex-m4f，text）：fast interp **58.9 KB**、AOT runtime **29.4 KB** | WAMR README |
| `wamrc` 支援 **riscv32 / riscv64 / xtensa / arm / aarch64 / x86** | `wamr-compiler/main.c` |
| `wamrc` 對 riscv32 的預設 ABI 是 **ilp32d** | 同上（help 文字） |
| 支援 XIP（AOT code 直接從 flash-mapped 區域執行） | WAMR README feature list |

**四個關鍵推論：**

1. **晶片無關**。你兩個 port（S3 = Xtensa、P4 = RISC-V）吃**同一份 `.wasm`**；AOT 在 host 上按 target 各編一份。用 E 路線的話，每個效果要編兩次、而且要跟 IDF 版本對齊。
2. **沙箱 = 你本來就想要的規則**。`architecture-flow.md` 寫「effects 不可以直接碰硬體」—— wasm 模組**在結構上就碰不到**，它只能讀寫自己的線性記憶體、呼叫你開給它的 import。這條規則從「大家要記得」變成「做不到」。
3. **記憶體有界**。線性記憶體的大小在載入時就定死（wasm 是 64 KiB page 為單位），`max` 可以夾。效果再爛也只會撐爆自己那 64 KB，不會吃到 frame pool。
4. **AOT 是可選的**。同一個 `.wasm`，interp 先上線，AOT 之後再開（一行 config）。這讓 P0 的風險降到「跑得慢」而不是「跑不動」。

**代價**：

- 工具鏈三段（tinygo → wamrc → pack），要養 `tools/fx.py`。
- **AOT 檔格式與 runtime 版本綁定**：`wamrc` 2.4.0 產的 `.aot` 要配 runtime 2.4.0。版本要釘死並寫進 `.fxe` 的 header。
- Espressif 的 component 是 1 年前（v2.4.0），**你的 IDF 是 v6.0.3**。這是 §9 的第 1 號風險，不是理論風險。
- `wamrc --target=riscv32` 預設 `ilp32d`，但 **P4 沒有 D 延伸（只有單精度 F）** → 必須 `--target-abi=ilp32f`（或 `ilp32` 走軟浮點，慢）。這一條寫錯的症狀是「AOT 載入就 illegal instruction」，很容易誤診成 runtime bug。

#### 4.4.1 wasm 是什麼（給寫 C 的人）

五句話：

1. **它是位元碼，不是機器碼。** 跟 JVM bytecode 同類，但設計得更小、更安全。
2. **它是堆疊機。** 運算元放堆疊上，`i32.add` 彈兩個推一個 —— 所以編碼很密（沒有暫存器編號要記）。
3. **它沒有指標。** 整個世界是一塊連續的 *linear memory*，只能用 offset 存取。**沙箱就是這樣成立的**：越界會 trap，不是踩到別人的記憶體。
4. **它是結構化控制流。** 沒有 `goto`，只有 `block` / `loop` / `if`。所以載入時可以一次驗證完（< 1 ms），執行期不用再檢查型別。
5. **它是可攜的。** 同一份 `.wasm`，x86 / ARM / RISC-V / Xtensa 都能跑 —— 你的 P4 與 S3 吃同一份。

```text
你的 Go 碼 ──tinygo──► .wasm ──wamrc──► .aot
              (PC)              (PC)

裝置：載入 .wasm → 直譯器逐條執行     （慢、小、跨晶片）
      載入 .aot  → 直接執行機器碼      （快、大、綁架構）
```

`.aot` 不是「另一種 wasm」，是**同一份程式的機器碼版本，在 PC 上編好**。裝置端不做編譯，只做載入。

#### 4.4.2 為什麼 `.aot` 比 `.wasm` 大

直覺會以為「原生＝最精簡」，其實相反：**wasm 為了「傳輸」優化（小），AOT 為了「執行」優化（快），兩個目標不同，不會同時最小。**

用 `tools/poc/fx_poc/fx.wasm`（367 B）裡的一段當例子：

```wat
local.get $d2      ; 2 bytes   (0x20 + 索引)
i32.const 6        ; 2 bytes   (0x41 + LEB128)
i32.shr_u          ; 1 byte    (0x76)
```

5 bytes 做三件事。編成 RISC-V（RV32 每條指令固定 4 bytes）：

```asm
lw   a0, 12(sp)    ; 4 bytes   ← $d2 從堆疊載進暫存器
srli a0, a0, 6     ; 4 bytes
sw   a0, 12(sp)    ; 4 bytes   ← 存回去
```

12 bytes 做同一件事 —— 約 2.5 倍（這是值落在堆疊上的情況；暫存器排得好會少一些）。放大來自四處：

| 來源 | wasm | 機器碼 |
|---|---|---|
| 指令寬度 | 1 byte opcode | RISC-V 固定 4 bytes |
| 常數 | LEB128，1–5 bytes | `lui` + `addi`，8 bytes |
| 區域變數 | 編號索引，2 bytes | 暫存器不夠就 `lw`/`sw`，每次 4 bytes |
| 函式呼叫 | `call 12`，2 bytes | 前後導 + 定址，8–16 bytes |

**經驗值 1.5–3 倍**（Xtensa 是變長指令，倍率小一點；常數多的程式倍率大一點）。

#### 4.4.3 但算總帳，AOT 可能反而更小

因為 **runtime 也要算進去，而且方向剛好相反**：

| | runtime | 每個模組 |
|---|---|---|
| interp 模式 | 58.9 KB（直譯器大） | 10 KB |
| AOT 模式 | 29.4 KB（不用直譯器） | 20 KB |

（WAMR 官方 README 的 cortex-m4f text size；IDF 上會更大，比例不變）

| 效果數 | interp 總量 | AOT 總量 | 誰省 flash |
|---|---|---|---|
| 1 | 59 + 10 = **69 KB** | 30 + 20 = **50 KB** | AOT |
| 5 | 59 + 50 = 109 KB | 30 + 100 = 130 KB | interp |
| 20 | 59 + 200 = **259 KB** | 30 + 400 = 430 KB | interp |

**只有一個效果時 AOT 省；效果一多，直譯版省。** 而你要的是「很多效果隨時切換」—— 所以直譯版才是省的那個（更別說它還跨晶片）。

#### 4.4.4 所以：第一版只出 `.wasm`

| 理由 | 說明 |
|---|---|
| 效能夠 | 676 px 每像素 19,700 cycles 的預算（§2），直譯用不到 5% |
| 跨晶片 | 一份檔案，P4（RISC-V）與 S3（Xtensa）都吃 |
| 少一段工具鏈 | 不用裝 `wamrc`、不用管 AOT 版本綁定、不用踩 `ilp32f` 那個坑 |
| 之後加不用改東西 | `.fxe` 的 `flags` bit0 就是「有沒有 AOT」（見 [fx-loader.md](fx-loader.md) §3），有就用、沒有就退回直譯 |

**什麼時候才值得為某個效果單獨產 `.aot`**：實測 **> 8 ms/幀**（超過 33 ms 的四分之一）。在那之前，AOT 只是多一個依賴。

> 一句話：**`.wasm` 是「小 + 可攜 + 慢一點」，`.aot` 是「快 + 綁架構 + 大」。你要的三個條件（多效果、兩顆晶片、676 px）都指向 `.wasm`。**

### 4.5 E：原生 ELF 熱載入

`igrr/hotreload`（Espressif 的 Ivan Grokhotkov）做的是：把 component 標成 `RELOADABLE`，build 成 shared library，執行期從 flash / PSRAM 載入並重定位，HTTP 上傳後在安全點 swap。registry 的 targets 標了 `esp32p4`（README 的支援表只列 S2 / S3 / C3，P4 要驗）。

**它的優勢是真的**：原生速度、**可以直接呼叫你的 C API**（連 `nc_mem_take` 都不用包）、PSRAM 執行、零 marshalling。

**但它不適合當主線**：

| 問題 | 後果 |
|---|---|
| 沒有沙箱 | 一個越界寫入 → 整台重開，燈全黑 |
| 綁 IDF 版本 / ABI | 升 IDF 要重編所有效果 |
| 綁晶片 | S3 與 P4 各編一份（兩套 toolchain） |
| 只能在「安全點」重載 | 效果不能中途換，要等 callback 回來 |
| 官方定位是**開發期** | README 自己寫「不該留在 production」 |

**判斷句**：如果效果**只有你自己寫**、**只有一種板子**、**不在乎當機**，E 是最省力的（一天可動）。只要上面三條有一條不成立，就該走 D。

### 4.6 F：MicroPython + viper（你已經有的那條）

`@micropython.viper` 會把標了型別的函式編成機器碼，比直譯快「數倍」（官方沒有保證倍率）。但：

- **GC 還在**：`list` / `bytearray` 的配置時間不可預測 → 你的 buffer-manager 整份文件就是在解決這件事，走回去等於白做。
- **記憶體不可宣告**：mpy 給不了「這塊 2,704 B 一定在、而且位址固定」。
- **P4 的支援**與 viper 在 RV32 上的成熟度都要另外驗。

它是很好的**原型工具**（先在那邊把效果寫出來看畫面），不是 runtime。

---

## 5. 建議架構：三層，熱路徑只有一個函式指標

```text
                 ┌──────────── 主機（Mac / 網頁）────────────┐
                 │  tools/fx.py：build / pack / push          │
                 └───────────────┬───────────────────────────┘
                                 │ NC4 file service（staging → validate → swap）
┌────────────────────────────────▼─────────────────────────────────┐
│ device                                                            │
│                                                                   │
│  Tier 0  params ──── config.json / MODE_SET        （現在就有）    │
│  Tier 1  IR     ──── JSON 節點圖 → 原生 kernel      （第二個做）    │
│  Tier 2  wasm   ──── .fxe → WAMR（interp / AOT）    （第三個做）    │
│                            │                                      │
│                            ▼                                      │
│              ┌──────────────────────────┐                         │
│              │ PixelTask（core 1）       │                         │
│              │  fx->render(fid, t, span)│  ← 只有這一個呼叫         │
│              └────────────┬─────────────┘                         │
│                           │ handoff（BACK → READY）                │
│                           ▼                                       │
│              RenderTask（core 0）→ RMT / SPI / I2C                │
└───────────────────────────────────────────────────────────────────┘
```

**三層共用同一個 C 介面**：

```c
typedef struct {
    const char *name;
    uint32_t    abi;
    void      (*init)(nc_fx_ctx_t *ctx);
    void      (*render)(uint32_t frame_id, uint64_t time_us,
                        uint32_t span_off, uint32_t span_len);
    void      (*deinit)(nc_fx_ctx_t *ctx);
} nc_fx_ops_t;
```

Tier 1 的 `render` 是「走一遍節點圖」，Tier 2 的 `render` 是「`wasm_runtime_call_wasm` 一次」。**上層完全不知道差別** —— 這是整個設計的價值：你可以先做 Tier 1，之後把 Tier 2 塞進來而不動 pipeline。

`render` 的簽名不是新的 —— `architecture-flow.md` §4 已經寫了 `render(frame_id, time_us, span)`。這份文件只是把它變成「跨語言邊界也成立」的呼叫約定。

---

## 6. ABI（最重要的一節）

> 這一節比選語言重要。語言可以換，ABI 換了所有效果要重寫。

### 6.1 只有三個 export

```wat
fx_abi_version() -> i32            ; 必須回 0
fx_init(desc_ptr: i32) -> i32      ; 0 = OK；非 0 = 拒絕載入
fx_render(frame_id: i32, time_us: i64, span_off: i32, span_len: i32)
```

**為什麼只有三個**：每一次跨邊界呼叫都是 50–100 cycles，而且編譯器不能內聯。把「每像素一次呼叫」設計進去，就注定慢；把介面壓到「每幀一次、載入時一次」，剩下的都在 guest 自己的記憶體裡跑。

沒有 `fx_alloc`：**效果不能配置記憶體**。它要的東西在 `fx_init` 之前就配好了（見 6.3）。

### 6.2 descriptor：host 寫、guest 讀

固定佈局，全部 little-endian：

| offset | 欄位 | 意義 |
|---|---|---|
| +0 | `px_count` | 像素數（676） |
| +4 | `width` | 矩陣寬（條燈 = px_count） |
| +8 | `height` | 矩陣高（條燈 = 1） |
| +12 | `fb_off` | 幀緩衝在線性記憶體裡的 offset |
| +16 | `fb_bytes` | `px_count * 4` |
| +20 | `sc_off` | scratch 的 offset |
| +24 | `sc_bytes` | scratch 大小 |

**幀緩衝的格式是 canonical 的 `px × 4 B（R,G,B,W）`** —— 跟 `memory.md` §3 一致。轉成 WS2812 的 3 B 是 output 階段的事，效果不用知道。

### 6.3 緩衝契約：宣告 → 一次配 → 交 offset

這是把 `buffer-manager.md` 的 declare→commit 延伸到「後期載入」的關鍵。**問題**：commit 在開機就關窗了，效果卻是之後才載入的。

**解法**：開機時把整個 fx 子系統宣告成**一個** plan item，效果在裡面 bump：

```c
// 開機（在 nc_plan_commit 之前）
nc_plan_declare(&(nc_plan_decl_t){
    // 實際欄位見 nc_plan.h：service / name / level / cls / bytes / align
    .service = "fx", .name = "fx.arena", .level = 4,   // L4 = 最先被撇除
    .cls = NC_PLAN_PSRAM, .bytes = NC_FX_ARENA_MAX,     // 例如 512 KB
});
```

```text
載入 .fxe 時（執行期，但不再是「向系統要」）：
  ① 讀 manifest 的 bufs[]  → 加總 bytes
  ② 在已 commit 的 fx.arena 裡 bump 出一塊線性記憶體     ← 不會失敗以外的副作用
  ③ 建 WAMR module instance，linear memory 指到那塊
  ④ 寫 descriptor → fx_init()
  ⑤ 成功才 swap 進去；失敗就保留舊效果
```

**這樣不變量還在**：OS 層的配置只發生一次（開機），執行期只有「已配置區域內的切分」。效果塞不進去就是**明確拒絕載入**（回 `NC_ERR_NO_MEM` + 原因），不是中途撇除 —— 撇除是開機的決策，不是執行期的。

**換檔時記憶體怎麼辦**：兩個選項 —— 重用同一塊（快，但要靠模組自己清；`fx_init` 的第一件事就是 memset 自己那塊），或換一塊（乾淨，但要付一次 64 KB 的填零）。建議**重用**，因為 64 KB 的 reset 比重新 carve 便宜，而且不會把 arena 弄碎。

manifest 長這樣（跟著 `.fxe` 走）：

```json
{
  "name": "fire26", "abi": 0, "wamrc": "2.4.0",
  "bufs": [{"name": "frame", "kind": "frame"},
           {"name": "heat",  "kind": "scratch", "bytes": 2704},
           {"name": "mat",   "kind": "scratch", "bytes": 4096, "align": 32}],
  "fps": 30, "params": [{"name": "speed", "type": "f32", "def": 1.0}]
}
```

### 6.4 為什麼 framebuffer 要住在 guest 的線性記憶體裡

三個選項：

| 做法 | 每幀成本 | 每像素 host 呼叫 | 備註 |
|---|---|---|---|
| fb 在 host，guest 每次寫都呼叫 `set_px()` | 676 次呼叫 | 676 | 慢、醜、不能向量化 |
| fb 在 host，guest 寫自己記憶體再整塊 copy | 1 次 memcpy 2.7 KB | 0 | 可接受 |
| **fb 就是 guest 記憶體的一塊（採用）** | 0（輸出階段本來就要 copy 到 DMA） | 0 | 最簡單 |

第三個是白吃的午餐：`memory.md` 已經規定「RMT DMA 不能指 PSRAM」，所以**輸出階段無論如何都要有一次 2,704 B 的 copy**（PSRAM → DMA 槽）。效果直接寫在自己的線性記憶體裡，這次 copy 就順便完成了格式轉換，不用多一次。

copy 成本：2.7 KB @ PSRAM 約 40 MB/s ≈ **68 µs**，佔 33 ms 的 0.2%。不是問題。

### 6.5 參數（v1）

參數不要每幀傳指標。做法：descriptor 後面接一塊 64 B 的參數區（16 個 f32），host 改參數時寫進去並呼叫 `fx_param(i)`（可選 export）。**沒宣告 `fx_param` 的效果就是不吃參數** —— 缺席即預設。

### 6.6 超時、掉幀、卸載

- 效果跑在自己的 task（低優先序），**不是**在 FrameClock 的 callback 裡。
- 超時：watchdog 每幀檢查；超過 N 幀（例如 3）→ 卸載並回上一個效果。
- 掉幀的語義是現成的：`architecture-flow.md` 已經寫「miss deadline 就重播上一幀」。**效果算不完 = 掉一幀**，不需要新機制。
- 主動終止：WAMR 有 `wasm_runtime_terminate()`。**但它在 AOT 模式下是否生效要驗**（§9-7）。保險做法是「不靠它」：超時就把整個 task 砍掉、module instance 作廢，反正效果是無狀態重載的。

### 6.7 版本與相容

- `abi` 同時存在於 manifest 與 `fx_abi_version()`；host 兩邊都比對，不符就拒絕。
- `.fxe` header 記 `wamrc` 版本與 target（`riscv32/ilp32f`、`xtensa`）。**AOT 檔不能跨 runtime 版本**，這是最容易在半年後咬人的欄位。
- 效果可以只帶 `.wasm`（interp 用）或同時帶 `.aot`（快路徑）。device 選哪個是 policy，不是格式。

---

## 7. 記憶體：每一塊放哪裡

| 項目 | 典型大小 | class | 為什麼 |
|---|---|---|---|
| WAMR runtime | 30–60 KB | INTERNAL | 常駐、要快 |
| 效果的 AOT code | 5–50 KB | **EXEC**（DIRAM） | 只有 DIRAM 能執行 |
| 線性記憶體（含 fb + scratch） | 64 KB 起（1 page 粒度） | PSRAM | 大、慢、不能 DMA —— 剛好符合 |
| 幀緩衝（canonical × 4 槽） | 2,704 × 4 = 10.8 KB | **DMA** | RMT 只能指內部 RAM |
| fx.arena（所有效果共享） | 128–256 KB | PSRAM | 一次 commit，執行期只 bump |

**P4 的特別之處**：`SOC_IRAM_LOW..HIGH = 0x4FF00000..0x4FFC0000`（768 KB），而且 DIRAM 的 I / D 是**同一個位址**（`MAP_DRAM_TO_IRAM(addr) = addr`）。意思是**內部 RAM 配出來就能執行**，不需要額外的 MMU 對映。這讓 AOT 在 P4 上比在 S3 上簡單得多。

S3 則是 0x3FC88000 起約 390 KB 的 DIRAM（你文件裡已量過），可用但沒那麼寬裕。**兩者都夠跑 1–3 個效果。**

> 想省 DIRAM 的話，WAMR 支援 **XIP**：AOT code 留在 flash-mapped 區直接執行。ESP32 的 flash 映射區本來就是可執行的，理論上可行。列為 §9-5，別當成已證實。

---

## 8. 工具鏈

### 8.1 Mac 上三段（② 是第一版可略的）

```bash
# ① 效果原始碼 → wasm（TinyGo；target 名依版本，wasm-unknown 或 wasm）
tinygo build -target=wasm-unknown -gc=leaking -scheduler=none \
             -o firmware/build/fire26.wasm ./effects/fire26

# ② wasm → AOT（可選，見 §4.4.4；每個 target 各一份，P4 必須 ilp32f）
wamrc --target=riscv32 --target-abi=ilp32f -o firmware/build/fire26.p4.aot firmware/build/fire26.wasm
wamrc --target=xtensa                      -o firmware/build/fire26.s3.aot firmware/build/fire26.wasm

# ③ 打包 + 上傳（走既有 NC4 file service）
tools/fx.py pack --manifest effects/fire26.json \
                 --wasm firmware/build/fire26.wasm --aot firmware/build/fire26.p4.aot \
                 -o firmware/build/fire26.fxe
tools/fx.py push firmware/build/fire26.fxe --port /dev/cu.usbmodem...
```

**② 不是必須的**：第一版只出 `.wasm`（§4.4.4）。等你某個效果實測 > 8 ms/幀，再為它單獨產 `.aot`，`.fxe` 的 flag 會自動選用。

`tools/fx.py watch effects/` 可以做成「存檔 → 重編 → 上傳 → 生效」，把迭代壓到 1–2 秒，逼近 mpy 的手感。

> `tools/fx.py` 是 P1 的產物，現在還沒有。上面三段指令是它要包起來的東西 —— 先手打驗證，再腳本化。

### 8.2 三個地雷（後兩個只在用 AOT 時）

| 地雷 | 症狀 | 解法 |
|---|---|---|
| `--target-abi` 用了預設 `ilp32d` | 載入就 illegal instruction | 明確 `ilp32f`（P4 只有單精度 FPU） |
| `wamrc` 與 runtime 版本不同 | 載入失敗 / 奇怪的行為 | 釘版本，寫進 `.fxe` header |
| TinyGo 的 GC | 幀內配置 → 不可預測的停頓 | `-gc=leaking` + 約定「init 之後不配置」 |

**最後一條要靠紀律**：ABI 沒有 `alloc`，所以效果「想配置」只能靠自己在 init 時把記憶體切好。這不是限制，是**把 buffer-manager 的規則延伸到 guest**。真的需要動態配置時，那是 ABI v1 的事，要另外設計（例如 `fx_alloc` 只在 init 期間合法，init 回傳後再呼叫就 trap）。

---

## 9. 未知與風險：P0 spike（一天）

**這是唯一會推翻 §0 的東西。** 按順序做，每項都有明確的過 / 不過：

| # | 要驗什麼 | 怎麼驗 | 過 |
|---|---|---|---|
| 1 | WAMR component 在 **IDF v6.0.3 + P4** 編得過 | `idf.py add-dependency espressif/wasm-micro-runtime^2.4.0`，build | 編得過、跑得起 `test_wasm.h` |
| 2 | 跑 `tools/poc/fx_poc/fx.wasm`（interp） | 板上載入、跑 120 幀 | **checksum 與 host 相同**（§10） |
| 3 | `wamrc` 產 P4 可用的 AOT | `--target=riscv32 --target-abi=ilp32f` | 載入 + 執行成功 |
| 4 | 真實速度 | 676 px 效果，interp vs AOT | AOT ≤ 2 ms/幀；interp ≤ 10 ms/幀 |
| 5 | AOT code 放哪 | `heap_caps_malloc(MALLOC_CAP_EXEC)`，印 free | 至少 64 KB 可執行記憶體 |
| 6 | S3 同一份 `.wasm` | S3 port 跑 interp（AOT 可選） | 行為相同 |
| 7 | 超時 / 終止 | 故意寫無窮迴圈的效果 | 3 幀內卸載、系統不掉幀 |
| 8 | 線性記憶體在 PSRAM | 1 page module + PSRAM heap | 建得起來、速度可接受 |

**第 1 項是最大的不確定性**：component v2.4.0 是 2025 年的東西，你的 IDF 是 v6.0.3。如果編不過，順序是「① 用 component 的 patch 分支 → ② 手動把 WAMR 當 third-party 放進 `components/` → ③ 才考慮換路線」。**不要因為第一天的建置失敗就放棄 D** —— 那是版本問題，不是路線問題。

---

## 10. 已經做出來的：ABI 的 host 參考實作

`tools/poc/fx_poc/` 是一個零依賴的 PoC，在 Mac 上把 §6 的 ABI 跑起來（不需要板子）。它做四件事：算佈局 → 寫 descriptor → `fx_init` → 每幀 `fx_render` 並讀回 fb。

### 10.1 基本：一個模組跑 120 幀

```text
$ node tools/poc/fx_poc/host.mjs tools/poc/fx_poc/fx.wasm --frames 120
layout      px=676 26x26  fb=64+2704  scratch=2784+2704  total=5488 B (memory 65536 B)
module A    fx.wasm (367 B wasm, abi 0)  fx_init ok
phase A     120 幀  avg 3.5 us  checksum 0x143b0b06
scratch[0]  120 次  (fx.wasm 被呼叫 120 次)
```

### 10.2 熱替換：第 120 幀換一份檔案（這就是「分離式固件」）

```text
$ node tools/poc/fx_poc/host.mjs tools/poc/fx_poc/fx.wasm --frames 240 \
       --swap tools/poc/fx_poc/fx2.wasm --at 120
swap @120   fx2.wasm → fx_init ok（496 us）；同一個 descriptor、同一組 offset，host 沒有重算佈局
phase A     120 幀  avg 3.4 us  checksum 0x143b0b06
phase B     120 幀  avg 2.4 us  checksum 0x22cd5444  (fx2.wasm)
```

![換檔前後（左：徑向脈衝；右：斜向掃描）](../../tools/poc/fx_poc/frame.png)

左邊是 A（徑向脈衝）、右邊是 B（斜向掃描）。**中間沒有重新啟動、沒有重配記憶體、沒有重算佈局** —— 只是把 `render` 的函式指標換掉。

### 10.3 壞檔：被拒絕，舊的繼續跑

```text
$ node tools/poc/fx_poc/host.mjs tools/poc/fx_poc/fx.wasm --frames 240 \
       --bad tools/poc/fx_poc/fxbad.wasm --at 120
swap @120   拒絕載入 fxbad.wasm（fx_init 回 1）→ 繼續用 fx.wasm，畫面不中斷
phase B     —（換檔被拒絕，全程都是 fx.wasm，沒有中斷）
```

這是「不需要燒錄」能夠成立的前提：**上傳壞東西的最壞結果是「沒換成功」，不是「變磚」**。ELF 熱載入給不了這個保證。

### 10.4 兩個附帶用途

- **conformance test**：效果只用整數運算的話，**host 與 P4 應該得到同一個 checksum**（§9-2 的驗收條件）。
- **換語言不換 harness**：`fx.wat` 是手寫的測試替身（**不是給人寫燈效的**）。等 TinyGo 的工具鏈好了，把 `fx.wasm` 換掉就行，`host.mjs` 一行都不用改。

> 誠實說明：PoC 是 node 的 `WebAssembly`，每個模組自帶 1 page（64 KB）。上板時線性記憶體由 host 從 `fx.arena` 切給模組（WAMR 支援），效果看到的仍然是同一組 offset —— 所以 ABI 一樣，記憶體政策不同。

---

## 11. 分期

| 階段 | 內容 | 估 | 驗收 |
|---|---|---|---|
| **P0 spike** | §9 的 8 項 | 1 天 | 全過 → 繼續；1/3/5 不過 → 回頭評估 C 或 E |
| **P1 ABI** | `nc_fx_ops_t`、descriptor、`tools/fx.py`、host harness | ~250 行 | 現有 harness 跑得動；ABI 文件化 |
| **P2 interp 上板** | WAMR component、fx.arena、載入 / 卸載 / 超時 | ~400 行 | 兩個效果可切換；無窮迴圈 3 幀內被踢掉 |
| **P3 AOT** | wamrc 進 build、`.fxe` 帶 aot、版本檢查 | ~150 行 | 同畫面、≤ 2 ms/幀 |
| **P4 IR tier** | 12 個節點 + 圖求值器 | ~800 行 | 用 IR 重寫 3 個現有效果；與 wasm 效果並存 |

**順序的理由**：P1 是純 host、不需要板子（跟你 `tools/hosttest/plan_test` 的做法一樣）；P2 才需要硬體；IR（P4）**可以跟 P0–P3 平行**，因為它不依賴 wasm。

---

## 12. 決策記錄（被否決的）

| 方案 | 為什麼不用 |
|---|---|
| **Go 本體**（`GOOS=js`） | runtime 2 MB+、GC、wasm 目標本來就慢；TinyGo 才是對的 |
| **TinyGo 直接編 ESP32 固件** | 那叫換 firmware，不是後期載入 |
| **自訂語言 + bytecode** | 3–5k 行 + 生態為零；Pixelblaze 能做是因為它是一間公司的主力產品 |
| **原生 ELF 當主線** | 無沙箱、綁 IDF、雙 ISA 各編一份；只適合「只有你、只有一種板」 |
| **Lua / LuaJIT** | 無 AOT（RISC-V MCU 上沒有 JIT）、有 GC |
| **eBPF** | 沒有動態記憶體（512 B stack + map），矩陣不現實 |
| **每次改效果就 OTA 整個 firmware** | 3 MB / 數秒 / 要重開機，vs 20 KB / 0.3 s / 不中斷；而且不能多效果駐留 |
| **把 framebuffer 留在 host、guest 用呼叫寫** | 每幀 676 次跨界呼叫；白花的 |
| **讓效果自己 malloc** | 等於把 GC 的問題重新引入；配置只能在 init |
| **一開始就全部產 `.aot`** | 綁架構（S3/P4 各一份）、多一段工具鏈、檔案還更大；只在 > 8 ms/幀 時才值得（§4.4.4） |

---

## 13. 待決事項

| # | 問題 | 建議 |
|---|---|---|
| 1 | 要不要「多效果同時駐留」（切換 < 100 ms） | 要，但上限 3 個；arena 256 KB |
| 2 | AOT 什麼時候啟用 | 預設只出 `.wasm`；某效果實測 > 8 ms/幀 才單獨加 `.aot`（§4.4.4） |
| 3 | 參數走 config.json 還是 MODE_SET | 走 config（可存檔），MODE_SET 只改 runtime 值 |
| 4 | 誰可以上傳效果 | 第一版只走 USB 隨身碟 / 開發工具；不對外網開放 |
| 5 | 單一效果線性記憶體上限 | 64 KB（1 page）起，量測後再談 128 KB |
| 6 | 超時幾幀後卸載 | 3 幀 |
| 7 | 效果檔要不要簽章 | 第一版不做（只有你能上傳）；對外開放前再說 |
| 8 | 換檔時 linear memory 重用還是換一塊 | 重用（模組自己 memset），見 §6.3 |
| 9 | 要不要做「效果庫」（多個 `.fxe` 駐留、按鍵切換） | 要，但排在 P3 之後 |

---

## 14. 參考

- WAMR（Bytecode Alliance）— <https://github.com/bytecodealliance/wasm-micro-runtime>；footprint 與 XIP 見 README
- Espressif 官方 component：`espressif/wasm-micro-runtime` v2.4.0 — <https://components.espressif.com/components/espressif/wasm-micro-runtime>（targets 含 esp32p4，AOT 預設開啟）
- `igrr/hotreload` v0.10.0（原生 ELF 熱載入）— <https://components.espressif.com/components/igrr/hotreload>
- TinyGo（Go 子集 → wasm / MCU）— <https://tinygo.org/>
- Pixelblaze Expression Language（自訂 DSL + bytecode 的商業先例）— <https://electromage.com/>
- ESP-IDF v6.0.3 原始碼（本機 `~/esp/esp-idf`）：`components/soc/esp32p4/include/soc/soc.h`（IRAM 範圍）、`components/heap/port/esp32p4/memory_layout.c`（`MALLOC_CAP_EXEC`）
- 專案內：[architecture-flow.md](architecture-flow.md) §4（frame pipeline / effects API）、[buffer-manager.md](buffer-manager.md) §4–§5（declare→commit）、[memory.md](memory.md) §3（為什麼燈效要 pool）
