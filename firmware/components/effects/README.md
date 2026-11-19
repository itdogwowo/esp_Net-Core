# components/effects/ — 效果器內容（CORE 平面）

> **一句話**：這裡放「內建效果」——把時間＋參數＋幾何變成 canonical 畫面。
> **框架**（契約、registry、program interpreter、波表）住 `components/sw/pixel/`，唔喺呢度。
> 規則權威：[docs/03_notes/behavior-taxonomy.md](../../../docs/03_notes/behavior-taxonomy.md)。
> 狀態：**2026-11-19 設計階段**。目前只有這份 README；第一個住戶是 `pixel_effects/`（M2，見規則 §9）。

---

## 三平面中的位置

| 平面 | 住邊 | 這裡的角色 |
|---|---|---|
| **CORE** | `firmware/components/` | ✅ **就是這裡**：內建效果（C） |
| **USER** | repo 根 `modules/effects/` → `.nce` | 外掛效果（同一個契約，唔同交付） |
| **DATA** | `/data/pixel/effects/*.json` | 效果參數＋畫波 program（由 CORE interpreter 解釋） |

同一件事（效果）有三種交付：

| 交付 | 住哪 | 重燒 | 速度 | 風險 | 誰寫 |
|---|---|---|---|---|---|
| DATA program（JSON 波形） | `/data/pixel/effects/` | ❌ | 中 | 最低 | 使用者 |
| **CORE 內建（C）** | **這裡** | ✅ | 最高 | 中（要 review） | 開發者 |
| USER `.nce`（wasm） | `modules/effects/` | ❌ | 較慢 | 最低（沙箱） | 使用者 |

---

## 這裡放什麼

| ✅ 放 | ❌ 唔放 |
|---|---|
| 具體效果：呼吸、波浪、單色、文字、轉場 | 效果框架（基類、registry、interpreter）→ `sw/pixel/` |
| 每個效果一個 `.c`＋`const` 表 | 環（ring）／編碼器 → `sw/pixel/` |
| 效果的參數結構（由 caller 提供） | 效果任務／節奏／核 → `tasks/pixel_task/` |
| host 測試用的確定性 frame | 指令驗證、回覆 → `action/pixel_action/` |
| | mapping／modes／playlist → DATA `/data/pixel/` |
| | 外掛 `.nce` → `modules/effects/` |

**判準**：拿掉硬件、排程、mapping，它仲有冇意義？有 → 效果。冇 → 它係 task／driver／mapping。

---

## 契約（六條，摘要）

效果只收到 `nc_fx_in_t { px, w, h, now_us, params }`，只寫 caller 交來的 `dst`
（`px × 4` B，R,G,B,W）。**效果只畫畫布，唔做 mapping、唔知 lane、唔寫環。**

1. **純**：同一 `(state, in)` → 同一 frame。
2. **無配置**：唔准 `malloc`／`nc_buf`／`nc_plan`。
3. **無 IO**：唔准 `ESP_LOG`（熱路徑）、driver、bus、檔案系統。
4. **無排程**：唔准 FreeRTOS／`esp_timer_start`／開 task。
5. **有界**：一幀要在預算內完成。
6. **寫滿**：`dst` 的 `px × 4` B 全部要寫。

完整形狀見規則文件 §3.4。

---

## 目錄形狀（目標）

```text
components/sw/pixel/            ← 框架（唔准有具體效果）
├─ include/nc_fx.h              ← 契約
├─ nc_px_fx_core.c              ← registry、衝突檢查
├─ nc_px_fx_program.c           ← DATA program interpreter、波表
├─ nc_pixel_q.c / nc_pixel_encode.c
└─ nc_pixel_layout.c            ← mapping（群組 → 全域 index）

components/effects/             ← 內容（就是這裡）
├─ README.md
└─ pixel_effects/
   ├─ CMakeLists.txt
   ├─ include/nc_pixel_fx.h
   ├─ nc_pixel_fx_breath.c
   ├─ nc_pixel_fx_wave.c
   └─ nc_pixel_fx_table.c       ← const 表
```

- **一個域一個 component**，唔係一個效果一個 component。
- 加一個 C 效果 = 加一個 `.c` ＋ 表加一行；**唔使改 task、唔使改環、唔使改 driver**。
- ⚠️ `components/effects/` 暫時**唔好加 CMakeLists.txt**：加了 IDF 就會當它是一個 component。
  落地時連 `pixel_effects/` 一齊加去 `firmware/CMakeLists.txt` 的 `_nc_extra_dirs`。

---

## 同 DATA／USER 的關係

| | CORE（這裡） | DATA | USER |
|---|---|---|---|
| 內容 | C 效果 | JSON `program` + params | `.nce` |
| 誰解析 | 編譯器 | CORE interpreter | WAMR（host 讀 guest framebuffer） |
| 環 header `src` | `NC_PX_SRC_BUILTIN` | `NC_PX_SRC_BUILTIN`（同一條路） | `NC_PX_SRC_NCM` |
| 幾時生效 | 重燒 | 下一幀（`ASSET_RELOAD`） | 下一幀 |

三者出來的都係 canonical frame，環與渲染器唔知分別。

---

## 現況

| 效果 | 現時位置 | 目標 |
|---|---|---|
| 呼吸／波浪／單色／色階／第一區 | `sw/pixel/nc_pixel_producer.c`（同框架混在一起） | `effects/pixel_effects/`（內容）＋`sw/pixel`（框架） |
| DATA program | 冇 | `/data/pixel/effects/*.json` |
| `.nce` | `modules/effects/`（fb 未接） | 接 canonical frame |
| 主機推送 | 只有 API | `action/pixel_action/`＋`NC_PX_SRC_HOST` |

遷移步驟見規則文件 §9（M1–M8）。
