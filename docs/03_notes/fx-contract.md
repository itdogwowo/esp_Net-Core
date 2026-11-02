# fx 模組契約：安全邊界與交接緩衝區

> **狀態**：設計（2026-11-02）。骨架已上板，執行引擎待接。
> **相關**：[fx-loader.md](fx-loader.md) §10（三層）、[effect-language.md](effect-language.md) §6（ABI v0）、
> [memory.md](memory.md)（heap class）、[buffer-manager.md](buffer-manager.md)（declare→commit）、
> [architecture-flow.md](architecture-flow.md)（frame pipeline）
> **程式**：`firmware/components/app/fx/`（`nc_fx.c`、`include/nc_fx_abi.h`）

---

## 1. 一句話

> **模組只能碰自己的線性記憶體；核心每幀只交出一個緩衝區，其餘一律碰不到。**

「彈性」來自「可以換模組」，不是「模組可以亂跑」。邊界愈窄，愈敢後期載入。

---

## 2. 三個邊界

| 類別 | 內容 | 怎麼保證 |
|---|---|---|
| **碰得到** | 自己的線性記憶體（1–2 page，64–128 KB）：descriptor、參數區、scratch、framebuffer | wasm 的 linear memory 就是它的全世界 |
| **碰不到** | `/data`、周邊暫存器、其他模組、host heap、其他 task 的變數 | 結構上做不到：wasm 沒有指標、沒有 syscall、v0 的 import 是 **0 個** |
| **唯一的門** | `fx_render(frame_id, time_us)` 的參數，與 host 在 render 返回後讀走 fb | host 只在 guest 返回之後才碰那塊記憶體 |

這正是 `architecture-flow.md` 那條「effects 不可以直接碰硬體」——從「大家要記得」變成「做不到」。

---

## 3. 記憶體佈局（`nc_fx_abi.h`）

模組的線性記憶體由 host 從 **已 commit 的 `fx.arena`** 切出，位址固定、32-byte 對齊：

| offset | 大小 | 內容 | 誰寫 |
|---|---|---|---|
| 0 | 32 | `nc_fx_desc_t`（28 B + padding） | host 載入時寫一次 |
| 32 | 64 | 參數區（16 × f32） | host 改參數時寫 |
| 96 | manifest 宣告（預設 4 KB） | scratch | guest |
| 96 + sc | `px_count × 4` | **framebuffer（canonical R,G,B,W）** | guest |

上限：線性記憶體 ≤ 128 KB、px ≤ 4096（fb ≤ 16 KB）。超過 → **拒絕載入**，不是中途撇除。
（2026-11-02 補上：這兩條現在真的在 `nc_fx_load()` 裡檢查了；先前只寫在文件與 `nc_fx_abi.h`，
程式漏了 —— 所以 `px=10000` 的檔案會被照收。）

**為什麼 fb 在 guest 裡**（`effect-language.md` §6.4）：S3 的 RMT DMA 不能指 PSRAM，
所以輸出階段本來就要有一次 `px×4` 的 PSRAM → DMA 槽 copy（2.7 KB ≈ 68 µs，佔 33 ms 的 0.2%）。
fb 直接住在 guest，這次 copy 順便完成格式轉換，不會多一次。

---

## 4. 交接時序（每幀）

```text
fx task（core 1）                                輸出階段（原本就有）
┌───────────────────────────────────────────┐   ┌──────────────────────┐
│ 1. 取 live（原子讀）                        │   │ 4. memcpy fb → DMA 槽 │
│ 2. 量 t0                                   │   │    （PSRAM → 內部）   │
│ 3. fx_render(fid, t_us)  ← guest 寫自己 fb  │──▶│ 5. RMT 送出去         │
│    量 t1，若 (t1-t0) > 預算 → strike++      │   └──────────────────────┘
└───────────────────────────────────────────┘
```

- **方向是單向的**：guest 寫 fb，host 只在 render 返回後讀。host 不會在 render 期間碰那塊記憶體。
- **沒有鎖**：live 指標用 release/acquire 交換，舊模組兩幀後才釋放（`fx-loader.md` §5）。
- **超支不中斷**：這幀照送（可能掉幀），但會記 strike；連續 3 次 → 標記 `bad` 並停止選用。

---

## 5. ABI 表面（v0）

| 項 | 內容 |
|---|---|
| **export（guest 必須有）** | `fx_init(desc_ptr)`、`fx_render(frame_id, time_us)` |
| **export（可選）** | `fx_param(i)` —— 沒宣告 = 不吃參數 |
| **import（host 提供）** | **0 個**（v0 刻意） |
| **回傳值** | 0 = ok；非 0 = 模組自報錯誤（記 log、不 swap） |
| **trap** | host 收到 false → 標記 `bad`，live 退回上一個或 NULL |

v0 唔開 import 是刻意的：開了 `log`/`time`/`file` 就等於把核心的表面積借給不信任的程式碼。
要能力就升 `abi`，舊模組不受影響（abi 不符 → 拒絕載入）。

### 5.1 兩種 payload 格式（同一個 `.fxe` 容器）

| 格式 | `flags` bit0 | `target_id` | 誰產生 | 特性 |
|---|---|---|---|---|
| **wasm**（預設） | 0 | `0`（通用） | tinygo | 任何晶片都吃；直譯 |
| **aot** | 1 | `1` = riscv32 **ilp32f**（P4）<br>`2` = xtensa（S3） | `wamrc`（PC 上） | 快、大、**綁架構** |

載入時的四條檢查（全部在進 WAMR 之前）：

1. `flags` 說 AOT 但 `target_id = 0` → 拒絕（`ESP_ERR_INVALID_ARG`）
2. `flags` 沒說 AOT 但 `target_id ≠ 0` → 拒絕（自相矛盾）
3. AOT 的 `target_id` ≠ 這顆晶片 → **拒絕並講清楚**（`ESP_ERR_NOT_SUPPORTED`）
4. 晶片架構未知（`nc_fx_arch_target() == 0`）→ 只收 wasm

第 3 條是重點：錯架構的 AOT 若直接丟進 WAMR，症狀是 **illegal instruction**，
很容易誤診成 runtime bug（`effect-language.md` §4.4 記過這個坑）。寧可載入前就拒絕。

> P4 一定要 `wamrc --target-abi=ilp32f`：P4 只有單精度 F、沒有 D 延伸，
> 用預設的 `ilp32d` 一樣會 illegal instruction。

---

## 6. 失敗模型（全部不影響核心）

| 事件 | 誰發現 | 處理 | 核心影響 |
|---|---|---|---|
| guest trap（越界、除零、unreachable） | WAMR | 標 `bad`、live 退回上一個 / NULL | 無（那一幀照送舊內容或黑） |
| 無限迴圈 | host 量測幀時間 | strike++；3 次 → 標 `bad` 並停用 | 幀率下降，但 task 沒死 |
| 線性記憶體不足 | 載入時 | 回 `NC_ERR_NO_MEM` + 原因，保留舊效果 | 無 |
| `.fxe` 壞（magic/abi/crc） | 載入時 | 拒絕，**不刪**（留給人看） | 無 |
| 卸載中仍有引用 | 兩幀計數 | `frame_id + 2` 後才釋放 | 無（延遲釋放） |
| arena 碎片 | 載入時 | 全部卸載後整塊 reset | 無（bump allocator） |

**不變量**：arena 用量 ≤ 宣告量；任何時刻 live 指標只指向已驗證的模組；
guest 的寫入範圍由 wasm 邊界夾住，host 只需信任「fb 那一段在範圍內」。

---

## 7. 狀態機

```text
        load(驗證通過)            select
 檔案 ──────────────► LOADED ──────────► LIVE ──(select 別個)──► RETIRED
                       │                  │                        │
                       │ unload           │ trap / 3×超支          │ 兩幀後
                       ▼                  ▼                        ▼
                     FREE               BAD ──(unload)──► FREE     FREE
```

`LOADED` 不代表能跑（沒有執行引擎時 `ok=0`）；只有 `LIVE` 才在幀迴圈裡被呼叫。

---

## 8. 分期計劃

| 期 | 內容 | 驗收 |
|---|---|---|
| **P0** ✅ | 契約文件 + `nc_fx_abi.h` + 骨架（arena / 實例表 / select / 兩幀釋放 / scan / status） | 板上載入 `.fxe`、狀態查得到 |
| **P1** ✅ | **host harness**：`tools/hosttest/fx_test/` —— `cc` 直接編 `nc_fx.c` + 假 backend，在 Mac 上跑狀態機 | ✅ 13 組 case 全過（壞檔×4 / trap / 超支 / 兩幀釋放 / arena 回收 / 掃目錄 / 實例表上限 / wasm·aot 格式） |
| **P2** | WAMR patch：補 `#include <sys/stat.h>`、`S_ISBLK`；`fx_init`/`fx_render` 接起來 | 一個最小 `.wasm` 在板上跑出正確 fb |
| **P3** | 輸出階段：fb → DMA 槽 copy + RMT；量幀預算 | 676 px 穩定 30 fps，render < 8 ms |
| **P4** | NC4 file API 上傳 `.fxe` → staging → validate → rename → 熱載入 | 不重啟換效果，壞檔不會變磚 |

P1 的價值：**在沒有 WAMR 的情況下也能證明狀態機與邊界正確**，而且能跑在 CI。

---

## 9. 待決

1. ~~**`nc_plan_decl_t` 對不上**~~ → 已解決（2026-11-02）：`effect-language.md` §6.3 的範例改成
   程式真的有的欄位（`service` / `name` / `level` / `cls` / `bytes` / `align`）。
2. **scratch 大小**：manifest 宣告 vs 寫死 4 KB。
3. **多模組同時 live**？目前設計是單一 live；若要「上層效果呼叫下層」需要 ABI v1。
4. **AOT 與 runtime 版本綁定**：`wamrc 2.4.0` 產的 `.aot` 要配 runtime 2.4.0，
   但 `.fxe` 檔頭沒有 runtime 版本欄位。目前靠 `abi` + `target_id` 擋一部分；
   正式做法是把 `wamrc` 版本寫進 manifest（`manifest_off/size` 已經用了，見 fx-loader.md §3.1）。
4. **參數區更新時機**：host 寫入時 guest 可能正在 render → 需要「下一幀生效」的雙緩衝（64 B × 2）。
