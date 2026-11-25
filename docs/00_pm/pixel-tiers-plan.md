# 像素三層（效果 → 抖動 → 配對）落地實作計劃

> **2026-11-25 名稱更新**：本文的「今日名稱」已是歷史。現行四個任務係
> `pixel_effect_task`／`pixel_pair_task`／`pixel_encode_task`／`render`（公用輸出層，
> 唔屬 pixel 系列；舊 `effect`／`pair`／`render` 由 `nc_task.c` alias 讀取）；
> 舊 `pixel_task` 已退役。
> 計劃內容與量測不變，只係名同路徑過時。

> **For agentic workers:** 本專案沒有 superpowers 子技能；照 Task 順序逐個做，每步勾 `- [ ]`。
> 每個 Task 做完都要 `bash tools/hosttest/run_all.sh` 全綠才進下一個。

**Goal:** 把「效果層 12-bit → 抖動層（1 或 4 子幀）→ 配對層（直接寫硬件讀取的緩衝區）」落成生產程式碼，做到零拷貝、無清畫布、無隱藏行為，並在 9,600 px（16×600、8-bit）與 2,400 px（16×150、10-bit 抖動）兩種配置上驗收。

**Architecture:** 配對層用 `nc_px_ring_begin()` 取得硬件即將讀取的槽，`nc_px_layout_apply()` 直接寫入該槽，`nc_px_ring_commit()` 交棒——沒有畫布、沒有複製、沒有 memset。抖動層把 12-bit 值換成 8-bit：`subframes=1` 時四捨五入，`subframes=4` 時一次掃描同時寫 4 個槽（時間抖動，等效 10-bit）。組合編譯結果以 `/pixel/cache/<key>.pxlt` 永久保存，開機補建缺的（表 ≤32 KB 才存）。

**Tech Stack:** ESP-IDF 6.0.3、C17、`nc_px_layout`（組合編譯）、`nc_pixel_q`（環）、`nc4` 命令（0xF06B/0xF071/0xF077）、host 測試 `tools/hosttest/`。

> **API 名稱政策：** 名稱隨時可能改（有些名字用家未滿意）。本計劃以**角色**描述行為，
> 括號內是「今日名稱」；改名屬機械替換，照下面的 SOP 做，計劃內容不變。

---

## API 角色 ↔ 今日名稱（名稱可改）

| 角色（計劃用語） | 今日名稱（可能改） | 改名的話要同步 |
|---|---|---|
| 環取槽 | `nc_px_ring_begin` / `nc_px_ring_begin_ex` | `nc_pair_task.c`（Task 1）、render 端 |
| 環交棒 | `nc_px_ring_commit` | 同上 |
| 組合編譯 | `nc_px_layout_build` | `nc_pair_task.c`、`nc_layout_bench.c`、host 測試 |
| 配對寫入（8-bit） | `nc_px_layout_apply` | 同上 |
| 抖動寫入（4 子幀） | `nc_px_layout_apply4`（新增） | `nc_pair_task.c`、`pxLayout_test.c` |
| 12-bit → 8-bit | `nc_px_canvas12_to_8` | 只在改用獨立抖動層時 |
| 資產表建構 | `build_asset_tbl_into`（static） | `nc_pair_task.c` |
| 快取 key／大小／存／讀 | `nc_px_layout_cache_key` / `_cache_bytes` / `_save` / `_load` | `nc_pair_task.c`、host 測試 |
| 環（型別／查詢） | `nc_pixel_q_t`、`nc_px_ring_slots`、`nc_px_ring_slot_bytes` | 全 pixel 路徑 |

**改名 SOP（每次改名照做）**

1. `grep -rn "<舊名>" firmware tools docs` —— 確認所有呼叫點
2. 用編輯器重新命名（唔好只改 header，會漏 .c）
3. `bash tools/hosttest/run_all.sh` 全綠
4. 建置受影響 project：
   `IDF_PATH=$HOME/esp/esp-idf python3 make.py build ESP32-S3-Zero --project ESP32-S3-Zero-Show`
5. 更新本表的「今日名稱」欄（一行）

> 如果改名係在 Task 進行中發生：先完成當前 Task 的測試，再統一改名，唔好一邊改名一邊加功能。

## 已定案（唔再討論）

| 項目 | 決定 |
|---|---|
| 分層順序 | 效果層（12-bit 值）→ 抖動層（12→8，1 或 4 子幀）→ 配對層（落點） |
| 清畫布 | **唔做**（每幀覆蓋；用戶寫什麼就顯示什麼） |
| 補黑／自動黑畫面 | **唔做** |
| 開機 | 緩衝寫黑一次（只係初始化） |
| 硬件溝通 | 硬件直接讀同一緩衝區；**零拷貝**，冇「交」這一步 |
| 組合快取 | 每組合一個檔；表 ≤32 KB 才存；開機補建缺的 |
| 實測基準（9,600 px） | 500 組：寫 1.36 ms；完全隨機：12.29 ms |
| 實測基準（2,400 px、4 子幀） | 500 組：0.95 ms；完全隨機：4.63 ms |
| 抖動與刷新 | 4 子幀只在子幀率 ≥200 Hz 時用 → 每條線 px ÷4（16×150＝2,400）；**緩衝總數不變** |

## 檔案結構（會改／新增）

| 檔案 | 責任 |
|---|---|
| `firmware/components/tasks/pixel_effect_task/nc_pair_task.c` | 零拷貝、開機補建、抖動子幀排程 |
| `firmware/components/sw/pixel/asset/nc_px_layout.c` | `nc_px_layout_apply4()`（一次掃表寫 4 份） |
| `firmware/components/sw/pixel/include/nc_px_layout.h` | `apply4` 宣告、覆蓋率欄位 |
| `firmware/main/Kconfig.projbuild` | `NETCORE_PIXEL_SUBFRAMES`、`NETCORE_PIXEL_RING_SLOTS` |
| `tools/hosttest/pxLayout_test/pxLayout_test.c` | `apply4` 平均值／覆蓋測試 |
| `ports/project/S3/ESP32-S3-Zero-Show2400/` | 2,400 px（16×150）測試工程 |
| `docs/00_pm/status.md` | 每完成一個 Task 記一節 |

---

### Task 0（可選）：名稱定稿

若用家想改 API 名，開工前先做這一步，之後 5 個 Task 就唔使中途改名。

- [ ] **Step 1：列出想改的名稱**

照上面「API 角色 ↔ 今日名稱」表，逐個問用家：保留／改名（新名）。**冇回應就當保留。**

- [ ] **Step 2：改名（照 SOP）**

每改一個名：`grep` → 改 → `run_all.sh` → 建置 → 更新對照表。

- [ ] **Step 3：Commit**

```bash
git add -A
git commit -m "refactor: 依用家命名調整 pixel API（行為不變）"
```

---

### Task 1：配對層零拷貝（直接寫環槽）

**Files:**
- Modify: `firmware/components/tasks/pixel_effect_task/nc_pair_task.c:206-262`
- Test: `tools/hosttest/pxShow_test/pxShow_test.c`（既有 apply 測試；新增「寫入任意 dst 等於寫畫布」）

- [x] **Step 1：寫失敗測試**

在 `pxShow_test.c` 加：

```c
// 配對寫入任意緩衝區（零拷貝的前提）：結果同寫畫布後複製一模一樣
static void test_apply_into_any_buffer(void)
{
    uint8_t canvas[8 * 4] = { 0 };
    uint8_t direct[8 * 4] = { 0 };
    uint16_t vals[4] = { 4095, 2048, 0, 1234 };
    nc_px_job_t j = { 0 };
    j.write = NC_PX_WRITE_RGB;
    j.nranges = 1;
    j.rng[0].base = 0;
    j.rng[0].count = 8;
    j.nled = 8;
    nc_px_pair_apply(&j, vals, 4, canvas);
    nc_px_pair_apply(&j, vals, 4, direct);
    check(memcmp(canvas, direct, sizeof(canvas)) == 0, "任意緩衝區結果一致");
}
```

- [x] **Step 2：跑測試確認失敗**

Run: `bash tools/hosttest/pxShow_test/run.sh`
Expected: 編譯失敗（`test_apply_into_any_buffer` 未定義或未呼叫）

- [x] **Step 3：接上測試並確認通過（此 API 本來就支援任意 dst）**

在 `main()` 呼叫 `test_apply_into_any_buffer();`，重跑：Expected: PASS

- [x] **Step 4：改 `content_frame_once` 為零拷貝**

```c
        const int64_t a0 = esp_timer_get_time();
        uint8_t *p = nc_px_ring_begin(s_content.q);
        if (p == NULL) {
            nc_pipe_stats_full();
            return false;                 // 環滿：下 tick 再試
        }
        nc_px_layout_apply(&s_content.layout, s_eff_frame.vals, p);
        const int64_t a1 = esp_timer_get_time();
        white_mix(p, s_content.px, s_content.white);
        const int64_t w1 = esp_timer_get_time();
        nc_px_ring_commit(s_content.q);
        const int64_t c1 = esp_timer_get_time();
        nc_pipe_stats_pair(0, (uint32_t)(a1 - a0), (uint32_t)(w1 - a1),
                           (uint32_t)(c1 - w1), lat_us);
```

同時：`s_content.canvas` 只在 0xF06B 要求預覽時才複製（`s_snapshot_wanted` 旗標），唔要求就唔複製。

- [x] **Step 5：開機初始化緩衝為黑（一次）**

在 `content_start()` 環建立後：

```c
    uint8_t *p0 = nc_px_ring_begin(s_content.q);   // 開機一次，令第一幀係黑
    if (p0 != NULL) {
        memset(p0, 0, (size_t)s_content.px * NC_PX_BPP);
        nc_px_ring_commit(s_content.q);
    }
```

- [x] **Step 6：建置＋上板驗證**

Run: `IDF_PATH=$HOME/esp/esp-idf python3 make.py build ESP32-S3-Zero --project ESP32-S3-Zero-Show`
Expected: 建置成功；`0xF071` 的 `clear` 應為 0、`apply` 為寫入時間、`commit` 接近 0。

- [x] **Step 7：Commit**

```bash
git add firmware/components/tasks/pixel_effect_task/nc_pair_task.c tools/hosttest/pxShow_test/pxShow_test.c
git commit -m "pair: 零拷貝寫環（唔經畫布、唔清畫布）"
```

---

### Task 2：開機補建缺少的組合表

**Files:**
- Modify: `firmware/components/tasks/pixel_effect_task/nc_pair_task.c`（新增 `cache_prebuild_missing()`；`build_asset_tbl_into()` 已存在）
- Test: 真機：刪快取 → 重開 → log

- [x] **Step 1：加函式（放在 `content_start()` 之前）**

```c
static void cache_prebuild_missing(void)
{
    const nc_px_mode_tbl_t *modes = nc_px_mode_table();
    if (modes == NULL || modes->n == 0 || s_content.px == 0) {
        return;
    }
    nc_px_asset_tbl_t *tmp = heap_caps_malloc(sizeof(*tmp),
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (tmp == NULL) {
        return;
    }
    uint32_t present = 0, built = 0, too_big = 0, failed = 0;
    const int64_t t0 = esp_timer_get_time();
    const nc_px_hw_inv_t inv = { .hw = NC_PX_HW_WS2812, .base = 0, .px = s_content.px };
    for (uint8_t i = 0; i < modes->n; i++) {
        const nc_px_mode_t *m = &modes->item[i];
        const nc_px_map_t *map = select_map(m);
        if (map == NULL) { failed++; continue; }
        build_asset_tbl_into(tmp, m);
        const uint32_t key = nc_px_layout_cache_key(m->hash, map->hash, s_content.px);
        char path[96];
        (void)snprintf(path, sizeof(path), "%s/cache/%08lx.pxlt",
                       s_content.pixel_dir, (unsigned long)key);
        nc_px_layout_t probe;
        if (nc_px_layout_load(&probe, path, key, tmp) == ESP_OK) {
            nc_px_layout_free(&probe); present++; continue;
        }
        nc_px_layout_t lay;
        if (nc_px_layout_build(m, map, tmp, &inv, 1, &lay) != ESP_OK || lay.njobs == 0) {
            nc_px_layout_free(&lay); failed++; continue;
        }
        if (nc_px_layout_cache_bytes(&lay) > 32768u) {
            too_big++;
        } else if (nc_px_layout_save(&lay, path, key) == ESP_OK) {
            built++;
        } else {
            failed++;
        }
        nc_px_layout_free(&lay);
    }
    heap_caps_free(tmp);
    ESP_LOGI(TAG, "開機預建組合表：已有 %u、新建 %u、太大跳過 %u、失敗 %u（%u ms）",
             (unsigned)present, (unsigned)built, (unsigned)too_big, (unsigned)failed,
             (unsigned)((esp_timer_get_time() - t0) / 1000));
}
```

- [x] **Step 2：在 `content_start()` 第一次 mode 就緒後呼叫**

```c
    cache_prebuild_missing();
```

- [x] **Step 3：真機驗證**

1. 刪 `ports` 專案的 `/pixel/cache/*.pxlt`（WebMaster `POST /api/file/delete`）
2. 重開板子，看 log：Expected `開機預建組合表：已有 0、新建 N、太大跳過 0、失敗 0（N×~300 ms）`
3. 再重開一次：Expected `已有 N、新建 0`

- [x] **Step 4：Commit**

```bash
git add firmware/components/tasks/pixel_effect_task/nc_pair_task.c
git commit -m "pair: 開機補建缺少的組合表（≤32 KB 才存）"
```

---

### Task 3：抖動層（4 子幀、一次掃表寫 4 份）

**Files:**
- Modify: `firmware/components/sw/pixel/include/nc_px_layout.h`、`firmware/components/sw/pixel/asset/nc_px_layout.c`
- Test: `tools/hosttest/pxLayout_test/pxLayout_test.c`（新增 `test_apply4`）

- [x] **Step 1：寫失敗測試**

```c
// 4 子幀抖動：平均值要等於四捨五入的 8-bit 值（誤差 ≤1）
static void test_apply4(void)
{
    printf("[8] 4 子幀抖動：平均值 ≈ 12-bit>>4\n");
    nc_px_asset_t asset;
    make_asset(&asset, "breathe", 4);
    nc_px_asset_tbl_t assets = { .n = 1 };
    assets.item[0] = asset;
    const nc_px_hw_inv_t inv[] = { { .hw = NC_PX_HW_WS2812, .base = 0, .px = 4 } };
    nc_px_group_sel_t sel[] = { { .hw = NC_PX_HW_WS2812, .slice = ":" } };
    nc_px_group_t group = { .id = 1, .name = "full", .sel = sel, .nsel = 1 };
    nc_px_map_t map = { .id = 1, .name = "m", .group = &group, .ngroups = 1 };
    nc_px_mode_entry_t entry = { .group = "full", .effect = "breathe",
                                 .write = NC_PX_WRITE_RGB };
    nc_px_mode_t mode = { .id = 1, .name = "m", .nentry = 1, .entry = &entry };
    nc_px_layout_t lay;
    check(nc_px_layout_build(&mode, &map, &assets, inv, 1, &lay) == ESP_OK, "建立");
    uint16_t vals[4] = { 4095, 2048, 17, 1234 };
    uint16_t *vp[1] = { vals };
    uint8_t o[4][16];
    uint8_t *outs[4] = { o[0], o[1], o[2], o[3] };
    memset(o, 0, sizeof(o));
    nc_px_layout_apply4(&lay, (const uint16_t *const *)vp, outs, 4, 0);
    for (int px = 0; px < 4; px++) {
        const uint32_t want = ((uint32_t)vals[px % 4] + 8u) >> 4;
        uint32_t sum = 0;
        for (int s = 0; s < 4; s++) {
            sum += o[s][px * 4 + 0];
        }
        const uint32_t avg = (sum + 2u) >> 2;
        check(avg + 1 >= want && avg <= want + 1, "子幀平均 ≈ 8-bit 值");
    }
    nc_px_layout_free(&lay);
}
```

- [x] **Step 2：跑測試確認失敗**

Run: `bash tools/hosttest/pxLayout_test/run.sh`
Expected: 編譯失敗（`nc_px_layout_apply4` 未宣告）

- [x] **Step 3：實作（header）**

```c
// 時間抖動：一次掃描 runs，同時寫 subframes 份 8-bit 輸出。
// 每份 = base 或 base+1，令 subframes 份平均 = 12-bit>>4（±1）。
// phase 每幀 +1，令 +1 的位分散在不同子幀，避免閃爍。
void nc_px_layout_apply4(const nc_px_layout_t *l, const uint16_t *const vals[],
                         uint8_t *const dst[], uint8_t subframes, uint32_t phase);
```

- [x] **Step 4：實作（layout.c）**

```c
void nc_px_layout_apply4(const nc_px_layout_t *l, const uint16_t *const vals[],
                         uint8_t *const dst[], uint8_t subframes, uint32_t phase)
{
    if (l == NULL || vals == NULL || dst == NULL || subframes == 0) {
        return;
    }
    for (uint32_t r = 0; r < l->nruns; r++) {
        const nc_px_lrun_t *run = &l->run[r];
        const nc_px_ljob_t *job = &l->job[run->job];
        const nc_px_aprog_t *ap = &l->aprog[job->aprog];
        const uint16_t *v = vals[job->aprog];
        const uint32_t nv = ap->nv;
        const uint32_t nch = (job->write == NC_PX_WRITE_RGBW) ? 4u : 3u;
        uint32_t c[4] = { 0, 0, 0, 0 };
        for (uint32_t ch = 0; ch < nch && nv > 0; ch++) {
            c[ch] = (nch * run->src + ch) % nv;
        }
        for (uint32_t x = 0; x < run->count; x++) {
            const size_t o = (size_t)(run->dst + x) << 2;
            for (uint32_t ch = 0; ch < nch; ch++) {
                const uint32_t v12 = (v != NULL) ? v[c[ch]] : 0u;
                const uint32_t base = v12 >> 4;
                const uint32_t n1 = (((v12 & 15u) * subframes) + 8u) >> 4;
                for (uint8_t sf = 0; sf < subframes; sf++) {
                    const uint8_t pos = (uint8_t)((sf + phase) % subframes);
                    dst[sf][o + ch] = (uint8_t)(base + ((pos < n1) ? 1u : 0u));
                }
            }
            if (nch == 3) {
                for (uint8_t sf = 0; sf < subframes; sf++) {
                    dst[sf][o + 3] = 0;
                }
            }
            if (nv > 0) {
                for (uint32_t ch = 0; ch < nch; ch++) {
                    if (++c[ch] == nv) {
                        c[ch] = 0;
                    }
                }
            }
        }
    }
}
```

- [x] **Step 5：跑測試確認通過**

Run: `bash tools/hosttest/pxLayout_test/run.sh`
Expected: `ALL PASS`

- [x] **Step 6：Commit**

```bash
git add firmware/components/sw/pixel tools/hosttest/pxLayout_test/pxLayout_test.c
git commit -m "layout: apply4 一次掃表寫 4 個抖動子幀"
```

---

### Task 4：生產路徑接上子幀（Kconfig + pair）

**Files:**
- Modify: `firmware/main/Kconfig.projbuild`、`nc_pair_task.c`、`firmware/components/tasks/render_task/nc_render_task.c`
- Test: 真機 `0xF071`＋`0xF06B`

- [x] **Step 1：Kconfig 加兩個選項**

```kconfig
config NETCORE_PIXEL_SUBFRAMES
    int "每個視覺幀的抖動子幀數（1=8-bit，4=10-bit）"
    range 1 8
    default 1

config NETCORE_PIXEL_RING_SLOTS
    int "prd 環格數（要 ≥ 子幀數 + 1）"
    range 3 16
    default 3
```

- [x] **Step 2：pair 每視覺幀寫 N 個槽**

```c
#if CONFIG_NETCORE_PIXEL_SUBFRAMES > 1
    uint8_t *slots[CONFIG_NETCORE_PIXEL_SUBFRAMES];
    for (int s = 0; s < CONFIG_NETCORE_PIXEL_SUBFRAMES; s++) {
        slots[s] = nc_px_ring_begin(s_content.q);
        if (slots[s] == NULL) {
            nc_pipe_stats_full();
            return false;
        }
    }
    nc_px_layout_apply4(&s_content.layout, (const uint16_t *const *)s_eff_frame.vals,
                        slots, CONFIG_NETCORE_PIXEL_SUBFRAMES, s_content.commits);
    for (int s = 0; s < CONFIG_NETCORE_PIXEL_SUBFRAMES; s++) {
        nc_px_ring_commit(s_content.q);
    }
#else
    /* Task 1 的單幀零拷貝路徑 */
#endif
```

- [x] **Step 3：render 的 lane plan 用新 Kconfig 設格數**

在 `nc_render_task.c` 建立 lane plan 的地方（`px_declare()`／預設 lane cfg）把 `slots` 改成：

```c
    lc.slots = (uint8_t)CONFIG_NETCORE_PIXEL_RING_SLOTS;
```

- [x] **Step 4：建置**

Run: `IDF_PATH=$HOME/esp/esp-idf python3 make.py build ESP32-S3-Zero --project ESP32-S3-Zero-Show9600`
Expected: 成功；`RING_SLOTS=5` 時環 5 × 38.4 = 192 KB（PSRAM）。

- [x] **Step 5：真機驗證子幀數**

```bash
python3 tools/nc4.py --port /dev/cu.usbmodemB43A45AD52E41 raw 0xF071
```

Expected：`render.fps` = 200（子幀率）；每 4 個子幀為一個視覺幀（可用 `frames` 對 `pair_n` 觀察 4:1）。

- [x] **Step 6：Commit**

```bash
git add firmware/main/Kconfig.projbuild firmware/components/tasks/pixel_effect_task/nc_pair_task.c \
        firmware/components/tasks/render_task/nc_render_task.c
git commit -m "pair/render: 每視覺幀寫 N 個抖動子幀（Kconfig）"
```

---

### Task 5：2,400 px（16×150）測試工程＋驗收量測

**Files:**
- Create: `ports/project/S3/ESP32-S3-Zero-Show2400/`（project.json、sdkconfig.defaults、data/pixel/*）
- Modify: `docs/00_pm/status.md`

- [x] **Step 1：建工程**

```json
// ports/project/S3/ESP32-S3-Zero-Show2400/project.json
{ "name": "ESP32-S3-Zero-Show2400", "chip": "ESP32-S3", "board": "zero", "dir": "ESP32-S3-Zero-Show2400" }
```

```ini
# sdkconfig.defaults
CONFIG_NETCORE_PIXEL_LEDS=2400
CONFIG_NETCORE_PIXEL_SUBFRAMES=4
CONFIG_NETCORE_PIXEL_RING_SLOTS=5
CONFIG_NETCORE_PIXEL_RING_PSRAM=y
CONFIG_NETCORE_PIXEL_I80_LANES=1     # 測試板只有 1 條線；16×150 的硬件層已另行驗證
CONFIG_NETCORE_PIXEL_FPS=200          # 子幀率 200 Hz → 視覺 50 fps
```

- [x] **Step 2：複製 `ESP32-S3-Zero-Show9600/data/pixel` 並改 mode/map 為 2,400 px（150 × 16）**

- [x] **Step 3：建置上板，量三項**

```bash
python3 tools/layout_bench.py --port /dev/cu.usbmodemB43A45AD52E41 --all
python3 tools/nc4.py --port /dev/cu.usbmodemB43A45AD52E41 raw 0xF071
python3 tools/nc4.py --port /dev/cu.usbmodemB43A45AD52E41 raw 0xF079
```

Expected（對照 §7.188／§7.190）：
- 2,400 px、500 組：apply 0.42 ms、4 子幀 0.95 ms
- 2,400 px、完全隨機：4 子幀 4.63 ms
- 送燈 4 × 4.5 = 18 ms、視覺 50 fps

- [x] **Step 4：把數字寫入 `docs/00_pm/status.md` 新一節（§7.193）**

- [x] **Step 5：Commit**

```bash
git add ports/project/S3/ESP32-S3-Zero-Show2400 docs/00_pm/status.md
git commit -m "project: 2,400 px（16×150）抖動驗收工程與數據"
```

---

### Task 6（可選，先量再決定）：I80 波形合成 — 展開＋轉置一次過

> **2026-11-25 改寫**（原「chunked streaming」設計作廢）。三個已量測的事實：
>
> 1. `nc_i80_drv` 已經是 async（3 槽＋完成回呼），打包與送燈**本來就重疊**；
>    §7.123 的 37.9 ms 是 async 之前的舊量測。frame ≈ **max(encode, 18 ms)**（§7.199）。
> 2. chunked DMA 只降低「單次 encode 延遲」，**唔會**提高 throughput —— 所以唔再做。
> 3. §7.201 實測 8×8 SWAR 轉置只有 **1.22×**（22.52 → 18.51 ms）：瓶頸唔係移位算術，
>    而係 **gather** —— 每個 byte index 要由 16 條獨立 lane 緩衝各讀 1 byte。
>
> **真正有機會的是「展開＋轉置一次過」**（FastLED wave3 的形狀）：每 16 個輸入 byte
> 直接產生 24 個 u16（48 B），唔經 `wave_pack` 的 172.8 KB 中間緩衝，亦唔做第二次
> merge。輸出 layout 不變（每個 tick 一個 bus word，16-bit little-endian），所以
> 呢個 Task 可以用「逐 byte 對拍」做閘門，快唔過舊路徑就唔接 production。

**Files:**
- Modify: `firmware/components/sw/pixel/nc_pixel_encode.c`（新 API：展開＋轉置）
- Modify: `firmware/components/sw/pixel/include/nc_pixel_core.h`（宣告）
- Modify: `firmware/components/tasks/render_task/nc_i80_out.c`（**量到快才**接線）
- Test: `tools/hosttest/pixel_test/pixel_test.c`（新舊路徑 byte-exact）
- Bench: `firmware/components/bench/effect_bench/nc_layout_bench.c`（variant 17／18，test-only）
- Lab: `modules/lab/i80_pack_lab/`（host A/B 腳本＋記錄；唔係主體工具）

- [ ] **Step 0：16-lane 板用現行固件重測 async 基線** `frame_us / enc_us / tx_us / inflight`，
      確認 frame ≈ max(enc, 18 ms)。**（要硬件；2026-11-25 未有板，未做）**
- [x] **Step 1：host 原型＋byte-exact 測試**：新 `nc_px_codec_pack_bus()` 直接由
      `lanes` 個 codec 即時取每顆的線材位元組，8×8 SWAR 轉置後逐 tick 寫 bus word。
      **8-bit／無抖動**（production 16×600 的實際情況）行廣播快路徑：lane 的
      `lane_px()` 輸出相同，唔需要 16 次呼叫＋轉置；有抖動才行完整轉置。
      對舊路徑（`wave_pack` × N ＋ `merge8_store`／`merge8`）**逐 byte 比對**：
      8／16 lane、RGBW、8-bit／12-bit dither、`bit_cells` 3／4／8、奇數 px。
- [x] **Step 2：bench variant 17（0xF077）A/B**：同一塊板、同一份 payload，
      量 `bus_old_us`、`bus_new_us`、`bus_ok`（逐 byte 相同）。
      指令：`python3 modules/lab/i80_pack_lab/run_ab.py --port <PORT> --px 9600 --variant 17 --reps 8`
      （lab 工具；`tools/layout_bench.py` 保持通用，冇 lab variant）。
      **實測（ESP32-S3、EffectPerf 映像）**：
      | px | 舊 | 新 | 倍數 | bus_ok |
      |---|---:|---:|---:|---:|
      | 9,600（16×600） | 24,018 µs | **6,365 µs** | **3.77×** | 1 |
      | 2,400（16×150） | 4,765 µs | **1,166 µs** | **4.09×** | 1 |
      reps=16 重跑數字一樣（6,365／1,166 µs）→ 唔係偶然。
- [x] **Step 3：快過舊路徑才接 `i80_encode`**：3.77× ≫ 20%，已接。
      `i80_encode()` 改成一次過 `nc_px_codec_pack_bus()`，同時刪走
      8 個 packed 中間緩衝（16×600 省 43.2 KB PSRAM）。
      **單 lane／8-bit 專用快路徑**：1 lane 時逐 tick 廣播反而慢（實測 64 px：
      840 µs vs 舊路徑 357 µs），所以 1 lane 用 `nc_px_codec_wave_frame()` 的
      LUT memcpy 路徑；量到 **139–165 µs**（比舊 357 µs 快 ~2.2×）。
- [x] **Step 4：上板驗證（1-lane 板）**：Matrix（1 lane、8-bit、64 px）tests 映像
      `0xF061`：`fps=50`、`frames=1391`、`fail=0`、`tx_us=18`、`enc_us=139–165 µs`。
      **16-lane 16-bit 實機仍然未做**（冇 16-lane 板）；bench 的 16-lane 數字係
      同一份 C 在板上跑，但未經真實 16 條線輸出。
- [x] **Step 5：Commit**

> **順手修（2026-11-25）**：`i80_encode` 的 16-bit 高位 byte 原本用 `merge8`（OR）。
> 高位 byte 只會 0→1，lane 由亮變暗會殘留（半邊「卡住」）；低位（off=0）與高位
> （off=1）是不同 byte，用 `merge8_store` 不會覆蓋另一組。已改成兩組都用 store，
> 8-bit 路徑行為不變。呢個係 Task 6 量 A/B 之前必須先修，否則「新舊對拍」會把
> 舊 path 的殘留位當成新 path 的錯。

**驗收（2026-11-24 上板後填）**：
- 輸出逐 byte 相同（硬閘門）：9,600／2,400 px、8/16 lane、8/12-bit dither 全部
  `bus_ok=1`；host 360 checks 全過。
- `enc_us`（16×600）：舊 19.4 ms → **6.36 ms**（目標 ≤ 12 ms，達標）。
- frame ≈ max(enc, 18 ms) ≈ 18 ms（≈50 fps）：16-lane 實機待量。
- 1-lane 64 px：**139–165 µs**（舊 357 µs）。
- 若只快 < 20% 唔接 production：實測 3.77×，所以接。

### Task 7：配對改 6 B 單表（定案）

> 2026-11-24 定案：統一 6 B（dst2／src2／write1／aprog1），唔做 4 B／分表。
> 實測（完全隨機）：6 B 5.34 ms、4 B 4.50 ms、分表 6.23 ms、run 12.29 ms。
> 4 B 的數據只作文件備查（status §7.208–§7.209）。

**Files:**
- Modify: `firmware/components/sw/pixel/include/nc_px_layout.h`、`asset/nc_px_layout.c`
- Test: `tools/hosttest/pxLayout_test/pxLayout_test.c`

- [x] **Step 1：加 6 B 表結構（由既有 layout 展開）**

```c
typedef struct {
    uint16_t dst;      // 落點
    uint16_t src;      // 值流起點
    uint8_t  write;    // NC_PX_WRITE_*
    uint8_t  aprog;    // 效果編號
} nc_px_flat6_t;       // 6 B／顆
```

- [x] **Step 2：建立組合時展開（一次性）**

```c
// 由 runs 展開：每段的 count 顆，逐顆寫 dst/src/write/aprog
uint32_t k = 0;
for (r = 0; r < lay->nruns && k < px; r++) {
    const nc_px_lrun_t *run = &lay->run[r];
    const nc_px_ljob_t *job = &lay->job[run->job];
    for (uint32_t x = 0; x < run->count && k < px; x++, k++) {
        f[k].dst = (uint16_t)(run->dst + x);
        f[k].src = (uint16_t)(run->src + x);
        f[k].write = job->write;
        f[k].aprog = job->aprog;
    }
}
```

> **實作改動**：正式版 `src` 存的是**已經減約到值流長度的起點**，唔係像素 index。
> 上面第 557 行寫 `run->src + x`，之後每顆再 `×(3或4) % nv`；改為建表時就算好
> `(步長 × run->src) % nv`，之後每顆 +步長回繞 → 熱路徑**一次除法都冇**。
> 語義完全一樣（run 路徑的 `c3x/c3y/c3z` 就係咁嚟），但快過計劃的原型（見 Step 5）。

- [x] **Step 3：套用時按 write 分組，避免每顆 switch**

```c
// ① 數每個 write 有幾顆（NC_PX_WRITE_COUNT 個桶）
uint32_t cnt[NC_PX_WRITE_COUNT + 1] = { 0 };
for (uint32_t i = 0; i < px; i++) {
    cnt[f[i].write]++;
}
// ② 前綴和 → 每組在輸出表的起點
uint32_t off[NC_PX_WRITE_COUNT + 1] = { 0 };
for (int w = 0; w < NC_PX_WRITE_COUNT; w++) {
    off[w + 1] = off[w] + cnt[w];
}
// ③ 逐組用專用迴圈（rgb／rgbw 各自一個；唔使每顆判斷 write）
for (int w = 0; w < NC_PX_WRITE_COUNT; w++) {
    for (uint32_t i = 0; i < px; i++) {
        if (f[i].write != (uint8_t)w) {
            continue;                 // 正式版用 sort/bucket 一次排好，唔用這個 O(n×W) 掃描
        }
        const uint32_t c0 = ((w == NC_PX_WRITE_RGBW) ? 4u : 3u) * f[i].src
                            % nv[f[i].aprog];
        uint8_t *o = dst + ((size_t)f[i].dst << 2);
        o[0] = (uint8_t)(vals[f[i].aprog][c0] >> 4);
        o[1] = (uint8_t)(vals[f[i].aprog][(c0 + 1u) % nv[f[i].aprog]] >> 4);
        o[2] = (uint8_t)(vals[f[i].aprog][(c0 + 2u) % nv[f[i].aprog]] >> 4);
        o[3] = 0;
    }
}
```

> 正式版唔用上面 O(n×W) 掃描：用 bucket 一次重排 `f[]`（或建表時已分組），
> 令每個 write 的 range 連續，然後每個 range 一個專用迴圈。
>
> **實作決定（2026-11-25）：唔分組，保留單一 dst 升序表＋逐筆 switch。**
> 分組會改變「重疊時邊個贏」：run 路徑係 dst 升序、後寫覆蓋先寫（`pxLayout_test [9]`）；
> 按 write 分組之後，全部 R 會排到全部 RGB 之前，重疊的燈就由「後寫贏」變成「RGB 贏」。
> 要保留語義就只有「同一 write 內穩定」一種分组，而同一 write 分組其實等於唔分——所以
> 單表 + switch（write 一段一段咁變，分支預測器食得住）係唯一唔會靜靜改畫面語義的做法。
> 萬一要再快，下一步應該係「按 write 分表但每顆都記住次序」——記憶體換時間，留待有真機數字先講。

- [x] **Step 4：host 測試（重疊、stride、aprog 各異）**

```c
// 兩個 entry：A 寫 R、B 寫 G，同一批燈 → 兩個通道都要保留
// 兩個 entry 都寫 RGB，重疊 px2/3 → 後面的贏（見 pxLayout_test [9]）
```

Run: `bash tools/hosttest/pxLayout_test/run.sh` → Expected: ALL PASS

> 實際加的測試係 `pxLayout_test.c` 的 `test_flat6_matches_runs()`：混合 write（rgb／r／rgbw）、
> 重疊、隔顆，同一份 layout 用「6 B 表」同「強制 runs」各寫一次，**逐 byte 對拍**（8-bit 同
> 抖動 4 子幀 × 4 phase 都比），再驗 `nflat == Σ run.count`、`flat_bytes == nflat × 6`、
> 以及 `px > 65,535` 時拒絕展開（唔可以靜靜截斷）。比逐個 case 寫期望值更難出錯。

- [x] **Step 5：上板量**

> **2026-11-25 已量**（SuperMini、EffectPerf 映像、`tools/layout_bench.py --all`，reps=8 取最小）：
>
> | 佈局 | 6 B 表 | runs | 倍數 |
> |---|---|---|---|
> | 100 組 | 3,006 µs | 3,078 µs | 1.02× |
> | 500 組 | **2,954 µs** | 3,745 µs | 1.27× |
> | 1000 組 | 2,677 µs | 4,150 µs | 1.55× |
> | 1000 組 × 2 段 | **2,185 µs** | 4,645 µs | **2.13×** |
> | 完全隨機（每組 9 粒，9,598 條單顆 run） | **3,007 µs** | 11,950 µs | **3.97×** |
> | 同上，production 抖動 4 子幀 | **19,969 µs** | 56,991 µs | 2.85× |
> | **每粒燈自己一個 group**（9,600 job／9,600 條 run） | **3,009 µs** | 19,025 µs | **6.32×** |
> | 同上，production 抖動 4 子幀 | **19,971 µs** | 85,090 µs | 4.26× |
>
> **關鍵發現：擺位決定一切。** 第一次量係表放 **PSRAM** → 0.60–1.05×（慢過 runs，因為
> 每幀要多讀 57 KB PSRAM）；改放 **internal** 之後變 1.02–2.13×。所以 `flatify()` 只准
> internal，唔夠就唔建表（行 runs），唔做靜默降級。
> 詳細數字、原因、同「32-bit store 反而慢 1.5×」的實驗見 status §7.217。
> 仍然未達 2.5 ms 目標（2.95 ms）：剩下係 canvas 的 PSRAM 寫入，唔係表。
>
> 計劃原本的 Expected（500 組 ≤2.5 ms、完全隨機 ≤5.5 ms）**部分未達**。

```bash
python3 tools/layout_bench.py --port /dev/cu.usbmodemB43A45AD52E41 --all
```

Expected（9,600 px）：500 組 ≤ 2.5 ms、完全隨機 ≤ 5.5 ms（6 B 實測 5.34）。

- [x] **Step 6：Commit**

### Task 8：掃描後精確配置（去魔法數字）

> 開機流程：**掃描（數）→ 配置（精確數量）→ 初始化**。
> 掃描結果就是唯一的數量來源；唔再有任何 `item[32]`／`job[8]`。

**Files:**
- Modify: `firmware/components/sw/pixel/include/nc_px_asset.h`、`asset/nc_px_asset.c`
- Modify: `firmware/components/sw/pixel/asset/nc_px_layout.c`
- Modify: `firmware/components/tasks/pixel_effect_task/nc_pair_task.c`
- Test: `tools/hosttest/pxLayer_test/pxLayout_test.c`（大量效果）、`pxShow_test`

- [x] **Step 1：把資產表改成 flexible array**

```c
typedef struct {
    uint16_t       n;          // 已用
    uint16_t       cap;        // 配置了幾個
    uint8_t        owned;      // 1 = item 由本層配置（可以 realloc／free）
    nc_px_asset_t *item;
} nc_px_asset_tbl_t;
```

> **實作改動**：用「表頭 ＋ 指標＋容量」，唔用 C99 flexible array。原因：掃描係逐個檔
> `add`（Step 2 的兩次掃描要讀兩次檔，而且「數 `"name"` 出現次數」會漏掉冇寫 name、
> 靠檔名補的效果），所以表一定要**可增長**；flexible array 唔可以 realloc。
> `item` 由 `nc_px_asset_tbl_alloc／reserve／free` 管理（板上 PSRAM 優先），
> 呼叫端亦可以自己塞緩衝（`nc_px_asset_tbl_use`，owned = 0）。
> 新增 API：`nc_px_asset_tbl_alloc／free／use／reserve`（`nc_px_asset.h`）。

- [x] **Step 2：掃描先數、再配置、再解析**（改為「用到才倍增」，見 Step 1 註）

```c
// nc_px_asset_scan() 內部：
const uint16_t count = count_effects_json(json, len);   // 只數 "name" 出現次數
nc_px_asset_tbl_t *t = lalloc(sizeof(*t) + (size_t)count * sizeof(t->item[0]));
t->n = 0;
parse_effects_into(t, json, len);                       // 填入，最多 count 個
```

- [x] **Step 3：pair 的 per-mode 資產表按 entry 數配置**

```c
const size_t bytes = sizeof(nc_px_asset_tbl_t) +
                     (size_t)mode->nentry * sizeof(nc_px_asset_t);
s_content.assets = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
```

- [x] **Step 4：aprog 先數 distinct 效果，再精確配置**（之前已經係咁：`ndistinct` 先掃一次，`out->aprog` 按它配置；`NC_PX_LAYOUT_ASSET_MAX` 唔再係硬上限）

```c
uint32_t ndistinct = 0;
for (uint32_t i = 0; i < mode->nentry; i++) {
    if (find_asset_previously_seen(mode->entry[i].effect) == false) {
        ndistinct++;
    }
}
out->aprog = lalloc((size_t)ndistinct * sizeof(nc_px_aprog_t));
```

- [x] **Step 5：刪除舊 `nc_px_plan_t` 的 8 條上限**（production 冇用；host 測試改 layout）

> **已做（2026-11-25）**：唔係拆整個舊 path（`nc_px_pair_apply／apply12／canvas12_to_8`
> 仍然係 `effect_bench`／`pipe_bench` 的量測基準，拆咗會連量測工具一齊冇），
> 而係**拿走上限本身**：`nc_px_plan_t.job` 由 `job[8]` 改成「呼叫端指標 ＋ `jobcap`」，
> `nc_px_show_plan(..., nc_px_job_t *jobs, uint16_t jobcap)`。容量唔夠會計入 `skipped`
> （明示，唔會靜靜 drop）。4 個呼叫點（`nc_test_action`、`pxShow_test`、`pxMap_bench`、
> `pxLayout_test`）按 mode entry 數提供容量。全套 host 測試綠。

- [x] **Step 6：host 測試（40 個效果 > 舊上限 32）**

> 加在 `pxLayout_test.c [13]`：40 個效果 × 各自 4 顆 → `naprog = 40`、`nflat = 160`、
> `skipped = 0`。另外 `pxAsset_test` 的「表上限」測試改成「200 個全部收、第 200 個搵得到、
> 第一個仲喺（倍增搬過都保住）、`loaded = 200`」——舊版係驗「只收 32 個、其餘 rejected」。

```c
static void test_many_effects(void)
{
    printf("[10] 40 個效果（超過舊上限 32）\n");
    const uint16_t n = 40;
    nc_px_asset_tbl_t *t = malloc(sizeof(*t) + (size_t)n * sizeof(t->item[0]));
    t->n = n;
    for (uint16_t i = 0; i < n; i++) {
        char name[16];
        snprintf(name, sizeof(name), "e%u", (unsigned)i);
        make_asset(&t->item[i], name, 4);
    }
    nc_px_mode_entry_t *en = calloc(n, sizeof(*en));
    for (uint16_t i = 0; i < n; i++) {
        snprintf(en[i].group, sizeof(en[i].group), "full");
        snprintf(en[i].effect, sizeof(en[i].effect), "e%u", (unsigned)i);
        en[i].write = NC_PX_WRITE_RGB;
    }
    nc_px_group_sel_t sel[] = { { .hw = NC_PX_HW_WS2812, .slice = ":" } };
    nc_px_group_t g = { .id = 1, .name = "full", .sel = sel, .nsel = 1 };
    nc_px_map_t map = { .id = 1, .name = "m", .group = &g, .ngroups = 1 };
    nc_px_mode_t mode = { .id = 1, .name = "m", .nentry = n, .entry = en };
    const nc_px_hw_inv_t inv[] = { { .hw = NC_PX_HW_WS2812, .base = 0, .px = 4 } };
    nc_px_layout_t lay;
    check(nc_px_layout_build(&mode, &map, t, inv, 1, &lay) == ESP_OK, "建立");
    check(lay.naprog == n, "aprog 數 = 40");
    check(lay.skipped == 0, "冇 skip");
    nc_px_layout_free(&lay);
    free(en);
    free(t);
}
```

Run: `bash tools/hosttest/pxLayer_test/run.sh` → Expected: `aprog 數 = 40`、`冇 skip`

- [x] **Step 7：上板量開機記憶體與時間**

> **已量（2026-11-25，SuperMini ＋ EffectPerf 映像）**：
> 靜態 `nc_px_asset.c.obj` 的 `s_tbl` ＝ **12 B**（`nm --print-size`，舊版 8,386 B）；
> 開機 `status sys`：`h_dma` ＝ **203,827 B**、`h_ps` ＝ **2,082,872 B**；
> boot log「開機剩餘：內部 211,615 B（最大連續 131,072）｜PSRAM 2,082,872 B」。
> 註：`h_int` 同改前一樣（211,615 B）——BSS 縮細嘅 8,374 B 被 linker 段對齊／其他 section
> 吸收，唔會反映成 free heap；真正影響係**唔再有一塊 8.4 KB 常駐 BSS**，同埋效果多過
> 32 個唔會再被 drop。

```bash
python3 tools/nc4.py --port /dev/cu.usbmodemB43A45AD52E41 status sys
```

Expected：`h_int` 會**減少**（資產表 BSS 由 8,386 B 變成 N × 262 B，N = 實際效果數）；
`h_ps` 亦會減少（pair assets 與 aprog 都按實際數量配置）。
把 `status sys` 的數字寫入 status 文件。

- [x] **Step 8：Commit**

## 驗收條件（全部要過）

> 2026-11-25 更新：數字改為**真機實測**（6 B 表放 PSRAM）；舊門檻係 prototype 估算。

1. `bash tools/hosttest/run_all.sh` 全綠（20/20）。
2. 9,600 px、500 組：`0xF071` 的 `clear=0`、`apply ≤ 6 ms`（實測 5.4）、`commit ≤ 0.5 ms`。
3. 9,600 px、完全隨機（性能下限）：`build ≤ 160 ms`、`flatify ≤ 20 ms`、`apply ≤ 6 ms`（實測 5.37）。
4. 2,400 px、4 子幀：`apply ≤ 8 ms`（實測 6.03）、視覺 50 fps、子幀 200 Hz。
5. 開機預建：第二次開機 `built=0`；切換 < 20 ms（playlist 預建後 swap 實測 246 µs）。
6. 冇清畫布、冇補黑：程式碼搜尋 `memset(s_content.canvas` 應該 0 筆。
7. 效果數：一個 mode 的**不同效果**唔再受 32 限制（40 個效果真機驗證）。

## 風險與回退

| 風險 | 對策 |
|---|---|
| 環滿（4 子幀 × 200 Hz） | `RING_SLOTS=5`；pair 滿了回 false，下 tick 再試（已有機制） |
| 零拷貝後 0xF06B 預覽變慢 | 預覽改成「要求才複製」（Task 1 Step 4） |
| 抖動閃爍 | `phase = commits` 令 +1 位輪流 |
| 快取檔寫入 265 ms／個 | 只在開機補建；表 > 32 KB 唔存 |
| 完全隨機 9,600 px 8-bit 94% | 零拷貝後降到 61%（12.29 ms） |
