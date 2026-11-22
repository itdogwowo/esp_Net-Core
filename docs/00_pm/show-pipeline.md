# 檔案驅動 Content 管線（production 8×8 Show）

> 2026-11-21。目標：**production 的 8×8 Matrix 睇 `/data/pixel` 四層內容**（effects／map／
> modes／registry），由主機上傳、開機載入、逐幀合成、送 64 顆 WS2812B；支援「80% 白 ＋
> 20% 效果」的 white mix；另加 `0xF06B SHOWFRAME`（回 `0xF06C`）給 Web UI 預覽。
>
> 同 EffectPerf 的分別：EffectPerf 只量效果管線、唔點燈；Show 是**真的出畫面**的 production
> 映像。生產者由 `pixel_task`（內建六個效果）換成三任務管線：
> **effect（波形值）→ pair（配對／white mix／寫環）→ render（編碼／I80）**。
> `pixel_task`／render／編碼器／環全部唔改（render 仍然只認 canvas）。
>
> **2026-11-22 更新**：production canvas 及 Show I80 packed 波形已固定 PSRAM；
> 所有 LED 數走同一條服務路徑，沒有 internal/PSRAM runtime 分支。完整量測、
> 500 組可變長度 run 的 8-bit／12-bit 成本見
> [psram-service-pipeline.md](psram-service-pipeline.md)。

---

## 1. 資料流

```
主機（Web UI／tools/show_upload.py）
  │  PUT /pixel/{effects,map,modes}/*.json、/pixel/registry.json
  ▼
/data/pixel/                      ← DATA 平面（用戶資產，放 data 分區；boot/OTA 不覆寫）
  │  開機 on_start：nc_px_layer_load_all(<fs_base>/pixel)
  ▼
sw/pixel 的四層表（nc_px_layer_scan）
  effects  → nc_px_asset_tbl_t   （program 波形；12-bit）
  map/*.json → nc_px_map_tbl_t   （group = 硬體型別 + Python slice；group／sel 數由檔案決定）
  modes/*.json → nc_px_mode_tbl_t（mode.map[] = group × effect × write × range；條目數由檔案決定）
  registry.json → nc_px_show_t   （auto_play + list[]）
  │  選 mode（list[0]）→ mode.mapping → map → nc_px_layout_build(mode, map, assets, inv)
  ▼
nc_px_layout_t（aprog[] 值流 ＋ job[] ＋ dst 升序 runs[]）
  │   ← group.sel → index → counting bucket → run 合併（見 pixel-layout-pairing.md）
  │
  ├─ pair 發佈 aprog[]（nc_pipe_aprog_publish）；effect 拿副本
  │
  ├─ effect 任務（core0，50 fps）：每條值流一個 wave table／pool（×2 雙緩衝）→
  │    nc_pipe_effect_frame_t {seq, plan_gen, naprog, nv[], vals*[]}（只發指標）
  │    pair 處理完一幀才 ack(seq)，effect 之後才重用該 slot
  │
  ├─ pair 任務（core1）：copy latest frame → nc_px_layout_apply() 順序掃 runs
  │    寫 canvas → white_mix → prd 環；layout 由 pair 擁有
  │
  ▼
prd 環（4 B/px = R,G,B,W；render 任務建立／登記）
  ▼
render 任務（nc_render_task）：取最新一格 → 編碼（ws2812 GRB）→ I80 → 64 顆燈（50 fps）
```

**邊個負責什麼**（同 §7.142 的架構決定一致）：

| 層 | component | 負責 |
|---|---|---|
| 檔案 → struct | `sw/pixel`（`nc_px_layer*`／`nc_px_asset*`） | 掃目錄、解析 JSON、建表（PC 可測） |
| mode → layout | `sw/pixel`（`nc_px_layout.c`） | group.sel 展開、range、dst 排序 run 表、預編譯 program |
| 波形值 | `effect` 任務（pipeline_task component） | 每條值流 evaluate program；雙 pool＋ack；50 fps |
| 配對／white mix | `pair` 任務（pipeline_task component） | 載四層、建 layout、run 寫入、white mix、寫環、`0xF06B` |
| 環 → 燈 | `tasks/render_task` | 編碼、DMA、送燈（完全唔知四層） |

## 2. `pair`＋`effect` 任務（`firmware/components/tasks/pipeline_task/`）

由 **`CONFIG_NETCORE_CONTENT`**（`depends on NETCORE_PIXEL`，default n）開關；`CONFIG` 關閉時
`.c` 整段 `#if` 掉、component 是空 object（同 `effect_bench` 同一模式）。component 用
`WHOLE_ARCHIVE`＋constructor 自己登記任務與指令表，`main.c` 不需要改。

任務宣告（production 8×8）：

| 欄位 | 值 | 說明 |
|---|---|---|
| pair | `pair`／`pair`；NORMAL／4／order 21；stack 4096／core1；`0xF06B` 在此 |
| effect | `effect`／`effect`；NORMAL／4／order 22；stack 4096／core0；50 fps |
| render | 既有 `render`；core1；只讀 prd 環 |

**on_start（開機一次）**：

1. 找回 render 的 **prd 環**（名字含 `"prd"`，即 `i80.prd`）；找唔到只 log warning，loop 閒著。
2. `nc_store_fs_base()`（`/data`；USB 模式是唯讀 `/ro`）＋ `/pixel`；
   **確保 `<base>/pixel/map`、`<base>/pixel/modes` 存在**（`mkdir(…, 0777)`，`EEXIST` 當
   成功，失敗只 warn）：FILE_BEGIN 直接 `fopen("<path>.part","wb")`、唔會建父目錄，
   所以新機第一次上傳 map／modes 會 `err_write_fail`。`main.c` 的 `seed_dirs()` 現在
   也有這兩個目錄，pair 再確保一次（唔存在時仍然可以上傳）→
   `nc_px_layer_load_all()` 掃 effects／map／modes／registry，log 四層數量。
3. 選 mode：registry `list[0]`；冇 registry／搵唔到 → mode 表第一個。
4. 選 map：`mode.mapping` → 第一條 entry 的 `group` 前綴（`matrix.full`→`matrix`）→ map 表第一個。
5. 建 inventory（8×8 Matrix = 一條 WS2812、base 0、px = 環 px）＋
   只把 mode 引用到的 asset 抄入本地表 → `nc_px_show_plan()`。
6. `heap_caps_malloc(px*4, MALLOC_CAP_INTERNAL)` 做 canvas（8×8 = 256 B）；log 選中
   mode／map／white／plan jobs／skipped。

**每幀**：如上面資料流；`step` 每成功 commit 一格就 +1。環滿（renderer 未還格）→
**唔加 step**、`vTaskDelay(1)` 再試。表頭寫 `src=NC_PX_SRC_BUILTIN`、`ts_us=esp_timer_get_time()`、
`drops=環滿次數`。

**Stack 注意（2026-11-21 修正）**：`nc_px_map_t` 2,618 B、`nc_px_mode_t` 568 B；
`on_start` 由 main task（原本 3,072 B）呼叫，map parser 放棧會 stack overflow → boot loop。
已把 `nc_px_layer_scan.c` 的 `m` 改 static，並在 Show project
`CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192`。呢個係第一次上板失敗的根因。

**失敗策略（唔好 boot loop）**：`/data` 未掛載、`/data/pixel` 缺失、effects/map/modes 解析
失敗、plan 冇有效 job、canvas 配置失敗 → **log warning 講明原因**，進入 fallback：loop 仍然
每幀填黑（`NC_PX_F_CLEAR`）寫入環，render 顯示黑；**不 crash、不卡 boot**。修好檔案重開機
即用真 plan。`0xF06B` 在 fallback 會回黑幀（`last_valid=1`）。

**white mix**（`mode` JSON 的 `"white"` 或 `"white_mix"`，0..100，預設 0，舊 JSON 相容）：

```text
out = (out * (100 - w) + 255 * w) / 100       # 整數除法；R/G/B/W 四通道都做
```

- `w=0`：完全唔混白（快路徑，直接跳過）。
- `w=80`：每通道底 = 204，再疊 20% 效果 → 8×8 是一片白，效果在白色上輕微流動。
- 對 `memset` 後的黑像素一樣做：`0 → 204`，所以 **`write` 冇寫到的像素／通道都會變白**，
  這是公式的定義（白底），唔是 bug。
- 缺欄位／超範圍（>100、負數）→ 當 0，不會令整個 mode 解析失敗。

**解碼 task 的棧**：`0xF06B` handler 的 hex 直接寫入 decoder 的回覆幀緩衝
（`nc4_decoder_reply_buf()`），**唔喺棧上開 512×4 B buffer**。最近一幀另有 2,048 B static
快照（512 px × 4 B），spinlock 保護（show loop 寫、decode task 讀）。

## 3. 指令 `0xF06B SHOWFRAME`（回 `0xF06C`）

協議：NC4。請求 payload **無參數**（有都會被忽略）；回覆是「最近一次 commit 的一幀」。

回覆 payload 格式（同其他 JSON 指令一樣：`[len_lo][len_hi][json]`）：

```json
{"px":64,"seq":1234,"mode":"white80","white":80,"apply_us":251,"hex":"<px*4 bytes 的 hex，小寫>"}
```

- `px` = 環的燈數（production 64）；`seq` = show 已成功提交的幀數（單調遞增，重開歸 0）。
- `mode`／`white` = 目前 mode 名與白混合百分比；`apply_us` = 最近一次 mode plan rebuild 的時間（µs）。
- `hex` = **4 B/px**：每個像素 R,G,B,W 各 1 byte，依像素次序。**64 px = 512 hex 字元**
  （256 bytes）。解析例：`hex[i*8 .. i*8+7]` = 第 i 顆的 R,G,B,W。
- `{"err":"nodata"}`：show 未開始／未有快照（例如 render 未起）。
- `{"err":"big","px":N}`：`px > 512`，或 hex 塞唔落 decoder 的回覆 JSON 上限
  `NC4_REPLY_JSON_MAX`（2,600 B；約 px ≤ 315）。production 64 px 永遠在限內。
- 指令表：`firmware/components/tasks/pipeline_task/nc_pair_task.c` 的
  `NC_ACTION_TABLE_DEF(nc_pair_task_table, "pair", …)`；`0xF06B` 喺 `status cmds` 見到
  主人是 `content`。

Web UI 只需高頻 poll `0xF06B`，把 `hex` 解成 8×8 RGB（第三、四通道是 W 與白底）就有即時
預覽；唔需要改 render／送燈。

## 3.1 指令 `0xF06D SHOWMODE`（回 `0xF06E`）

payload `[mode_id:u8]`；decode task 只驗 id 存在、寫入 `s_pending_mode` 並立即回
`{"ok":1,"queued":2,"mode":"white80","white":80}`。真正 plan rebuild 由 `content` loop
喺下一個幀邊界做，避免 decode task 同 pair task 同時改 `s_content.plan`／`assets`。
切換後 `step=0`、canvas 重用、`0xF06B` 的 `apply_us` 會更新。真機 8×8 量到
**226–251 µs**（plan 1 job、64 px）；9,600 px 會隨 plan jobs 增加，但仍遠低於一幀。

## 3.2 指令 `0xF06F RELOAD`（回 `0xF070`）

無參數；decode task 只設 `s_pending_reload`，pair loop 喺下一幀邊界
`nc_px_layer_load_all(pixel_dir)` 重新掃四層，再按**目前 mode 名**重建 plan
（名唔存在 → registry list[0] → mode 表第一個）。改 `/pixel` 檔案不用重開。
回覆 `{"ok":1,"queued":"reload"}`；真正完成後 `0xF06B` 的 `mode`／`white` 會反映新內容。

## 3.3 auto_play

`registry.json` 的 `auto_play=true` 且 `list` 多過一個 mode 時，pair loop 每
`mode.maxF` 幀（`maxF=0` 用 500 幀）按 list 次序換下一個；`maxF` 是 mode JSON 欄位。
手動 `0xF06D` 不會關閉 auto_play，下一次仍由目前 mode 在 list 的位置往下走。
production 目前的 `list=["breathe"]` 只有一個，所以不會切。

## 4. Project：`ports/project/S3/ESP32-S3-Matrix-Show/`

```text
project.json            hardware S3/ESP32-S3-Matrix；aliases show／matrix-show；
                        seed = hardware,system,network,tasks
sdkconfig.defaults      CONFIG_NETCORE_CONTENT=y、CONFIG_NETCORE_PXBENCH=n（不開測試）
data/config/tasks.json  render level4 affinity[0,1]；content level4 affinity[0,1]；
                        pixel level -1（唔跑內建六個效果）
data/config/{system,network,hardware,resources}.json  由 Matrix-LED-Textkit 抄（同一塊板）
data/pixel/             **主機端內容來源**（唔 embed 進 firmware）：
                        effects/effects.json、map/matrix.json、
                        modes/white80.json（white=80）、modes/color.json（white=0）、
                        registry.json（list=["white80","color"]）
```

`/data/pixel` 唔會由 firmware seed（main 只 embed `data/config/*.json`）；主機用
`tools/show_upload.py`（Web UI）上傳。上傳之前的第一次開機：pair log warning、
顯示黑，屬預期。

**建置**：

```bash
IDF_PATH=~/esp/esp-idf python3 make.py build ESP32-S3-Matrix \
    --project ESP32-S3-Matrix-Show --fresh
```

板子事實（64 顆 WS2812B、GPIO14、I80 pins、50 fps）全部在
`ports/hardware/S3/ESP32-S3-Matrix/sdkconfig.defaults`，project 只選「多編哪個功能」。

## 5. 驗證與排錯

**驗證狀態（2026-11-21）**

- `IDF_PATH=~/esp/esp-idf python3 make.py build ESP32-S3-Matrix --project
  ESP32-S3-Matrix-Show --fresh`：**成功**，image 694.8 KB（0xadb50，分區剩 55%），33–35 s；
  `CONFIG_NETCORE_CONTENT=y`、`CONFIG_NETCORE_PXBENCH`／`EFFECT_BENCH` 均 not set；
  `_nc_task_reg_pair`、`_nc_task_reg_effect`、`_nc_action_reg_nc_pair_task_table`、`nc_pair_task_table`
  三個 symbol 已入 ELF（任務＋指令表會由 constructor 自動登記）。唯一警告是既有的
  `bus/bus_net/nc4_line_net.c` unused-function／variable，同本改動無關。
- Host（x86，同一份 C 實作）用 Show project 的真實 `data/pixel` 內容跑完整管線：
  `nc_px_layer_load_all` → effects=1／maps=1／modes=2／list=2；選 `white80`（white=80）→
  `mode.mapping=matrix` → `nc_px_show_plan` jobs=1 skipped=0 px=64；200 幀 white mix 後
  R/G/B ≥ 204、W=204、step 之間有動畫；`color`（white=0）W 保持 0。全部 PASS。
- 既有 host 測試 `pxLayer_test`／`pxShow_test`／`pxScan_test` 全綠（white 欄位是純新增，
  舊 fixture 行為不變）；另加驗 `"white_mix"` 別名、>100／負數／字串 → 當 0、舊 JSON → 0。
- **已上真機**（2026-11-21）：`pair`／`effect`／render 跑住、`0xF06B` 回 64 px frame、
  min RGBA=[204,204,204,204]、max=[255,255,255,204]，80% 白 floor 全中；
  `0xF06D` 切 `color`（white=0）後 frame W=0，切回 `white80` 後 W=204；
  plan rebuild `apply_us` 226–251 µs。

開機 log（tag `content`）應該順序見到：

```text
content: pixel 樹：/data/pixel → effects=1 maps=1 modes=2 list=2
content: 選中 mode：white80（id=1）mapping=matrix white=80%；map=matrix（1 groups）
content: plan：px=64 jobs=1 skipped=0；寫入環 i80.prd（slots=3、4 B/px）
content: pair 就緒：64 px、prd 環 i80.prd
```

常見情況：

| 症狀 | 原因 | 處理 |
|---|---|---|
| `找不到 prd 環` | render 未起／tasks.json render=-1 | 開 render（level 4、affinity [0,1]） |
| `/data 未掛載` | data 分區初始化失敗 | `STATUS_SET System.fs_format=1`＋REBOOT（會清 data） |
| `四層載入失敗` | `/data/pixel` 未上傳／effects 全壞 | 上傳內容後重開 |
| `plan 冇有效 job` | mode 的 group／effect 名對唔上 | 檢查 map／effects 名；log 有 skipped |
| 燈全黑但 `plan 就緒` | white=0 且效果值此刻全 0 | 正常（波形的低點）；或換 mode |
| 0xF06B 回 `nodata` | pair loop 未提交過 | 等 render 起／檢查 fallback log |

## 6. 未做／限制

- 開機選 registry `list[0]`；`auto_play` 輪播已加（list>1 時每 `maxF`／預設 500 幀）。
  手動切換 `0xF06D SHOWMODE`（payload mode id）＋ Web UI「套用到裝置」按鈕。
- `0xF06F RELOAD` 已加：改 `/pixel` 後不用重開；Web UI 有「重新載入 /pixel」。
- `.nce` 外掛效果（`NC_PX_ASSET_NCE`）仍然唔會合成（program interpreter 只認六種段型別）。
- white mix 是整幀統一套用；per-mode 動畫／fade 未做。
- `0xF06B` 上限受 decoder 回覆 JSON（2,600 B）約束：>315 px 的預覽會回 `err=big`
  （production 64 px 冇影響）。
- `.nce` 合成仍未做；`0xF06B` 預覽上限 >315 px 未改。
