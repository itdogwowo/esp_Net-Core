# LCD_CAM 直接驅動（階段①）＋ 稀疏 I80 schema（階段②）實作計劃

> **給 agentic worker**：用 checkbox 追蹤；每個 Task 完成後 commit。硬件驗證的閘門唔可以跳。
>
> 相關設計：[pixel-i80-pin-schema.md](../03_notes/pixel-i80-pin-schema.md)（§8.2 三條路線、§8.3 影響面）、
> [pixel-output-backends.md](../03_notes/pixel-output-backends.md)。

**Goal**：S3 的 I80 輸出不再經 `esp_lcd`，只 route 真正接燈的 lane（Matrix：1 支 GPIO14），
**畫波（codec）／配對／環／任務零改動**；驅動通了之後，才改 config 讓 `data:[14]` 這種寫法成立。

**Architecture**：`nc_i80_drv_*` 這層介面 freeze；新增一個同介面的直接寄存器實作，
用 Kconfig `NETCORE_PIXEL_I80_DIRECT` 二選一編譯（esp_lcd 版保留作回退）。上層
（`nc_i80_out.c`／`nc_render_task.c`）只改「腳位怎麼傳」，buffer 契約不變。

**Tech Stack**：ESP-IDF v6.1.1（**WSL 內建置**）、C17、Kconfig、既有 host test、Matrix 板 OTA 驗證。

---

## 0. 環境（每次開工先做）

- [ ] `python tools/tenv.py brief` → 確認 `build/board/ota` runnable、板 `cid=54EC`
- [ ] 建置命令（**必須在 WSL 內**）：
  `bash -lc 'cd /mnt/c/Users/bl24847/Documents/code/git/esp_Net-Core && IDF_PATH=/home/user/esp/esp-idf python3 make.py build ESP32-S3-Matrix --project ESP32-S3-Matrix-LED-Textkit'`
- [ ] host test：`bash -lc 'cd /mnt/c/.../esp_Net-Core && bash tools/hosttest/run_all.sh'` → 21/21

## 1. 檔案結構（會碰什麼）

| 檔案 | 動作 | 責任 |
|---|---|---|
| `firmware/components/driver/drv_i80/include/nc_i80_drv.h` | 改 | cfg 加 `lane_pins`／`lanes`（esp_lcd 版仍用 `pins`） |
| `firmware/components/driver/drv_i80/nc_i80_drv.c` | 改小 | esp_lcd 版：忽略新欄位、行為不變 |
| `firmware/components/driver/drv_i80/nc_i80_drv_direct.c` | **新增** | 直接寫 LCD_CAM：時鐘／`i80_ctrl`／只 route lane／DMA／完成中斷 |
| `firmware/components/driver/drv_i80/CMakeLists.txt` | 改 | 依 Kconfig 二選一 `SRCS` |
| `firmware/main/Kconfig.projbuild` | 改 | 加 `NETCORE_PIXEL_I80_DIRECT`（depends on `NETCORE_PIXEL_I80`，S3 only） |
| `firmware/components/tasks/render_task/nc_render_task.c` | 改 | `i80_src()`：direct 路徑只填 `lane_pins`（`led->lanes` 支） |
| `firmware/components/tasks/render_task/nc_i80_out.c` | 改 | 傳 `lane_pins`／`lanes`；claim 只 claim 真 lane（direct） |
| `ports/project/S3/ESP32-S3-Matrix-LED-Textkit/sdkconfig.defaults` | 改 | 加 `CONFIG_NETCORE_PIXEL_I80_DIRECT=y` |
| `docs/00_pm/status.md`、設計稿 §8.3 | 改 | 實測數字與結論 |

**唔碰**：`nc_px_codec_*`、`nc_px_layout_*`、`nc_buf`、`pixel_*_task`、`render` 節拍、
`tools/ncportgen.py`（階段①）。

## 2. Task 1：Kconfig 開關（10 分鐘）

- [ ] 在 `firmware/main/Kconfig.projbuild`，`config NETCORE_PIXEL_I80` 之後加：

```
config NETCORE_PIXEL_I80_DIRECT
    bool "I80：直接寫 LCD_CAM 寄存器（唔經 esp_lcd，只 route 用到的 lane）"
    depends on NETCORE_PIXEL_I80
    default n
    help
      階段①：S3 專用。同 esp_lcd 版同一個 nc_i80_drv_* 介面，
      buffer 契約不變（每個 PCLK 一個 bus word），所以畫波層零改動。
      好處：wr／dc／未用 data 腳唔再需要真 GPIO（= FastLED 在 S3 的做法）。
      風險：cache writeback／完成中斷／PCLK 分頻要自己補；先 1 lane 8-bit 上板驗證。
```

- [ ] Matrix-LED-Textkit 的 `sdkconfig.defaults` 加 `CONFIG_NETCORE_PIXEL_I80_DIRECT=y`
- [ ] Commit：`feat(drv_i80): Kconfig 開關 NETCORE_PIXEL_I80_DIRECT`

## 3. Task 2：擴充 `nc_i80_drv_cfg_t`（15 分鐘）

- [ ] `nc_i80_drv.h` 的 cfg 改成：

```c
typedef struct {
    // esp_lcd 版：D0..D(width-1)、WR、DC（width=8 → 10 個；16 → 18 個）
    const int *pins;
    // direct 版：真正接燈的 lane 腳（lanes 個）；pins 可以 NULL
    const int *lane_pins;
    uint8_t    lanes;       // direct 版用；0 = 當 1
    uint32_t   pclk_hz;
    uint32_t   slot_bytes;
    uint8_t    slots;       // 2..NC_I80_DRV_SLOTS_MAX
    uint8_t    bus_width;   // 8 或 16（direct 版只影響每個 tick 出幾個 byte）
} nc_i80_drv_cfg_t;
```

- [ ] `nc_i80_drv.c`（esp_lcd 版）install 開頭加：
  `if (cfg->pins == NULL) { ESP_LOGE(TAG, "esp_lcd 版需要 pins（用 DIRECT 才只需 lane_pins）"); return ESP_ERR_INVALID_ARG; }`
- [ ] 建置 rc=0（行為不變）；`0xF071` 數字同之前一樣（回歸閘門）
- [ ] Commit：`refactor(drv_i80): cfg 加 lane_pins/lanes（esp_lcd 版行為不變）`

## 4. Task 3：直接驅動實作（1–2 小時，最難）

- [ ] 新檔 `nc_i80_drv_direct.c`：**只實作** `nc_i80_drv.h` 的同一組函式
      （`install/uninstall/ready/slots/slot_bytes/slot_buf/submit/slot_done/submitted/
      done_count/fail/inflight/pclk`），內部照 §附錄 A 的步驟：
  - [ ] `install()`：買/對時鐘（2.5 MHz）→ `LCD_CAM` 寄存器 → GPIO matrix route
        `lane_pins[0..lanes-1]`（**只 route 咁多**）→ 3 個 64-byte 對齊 DMA 槽
        （≤48 KB 用 internal；否則 PSRAM＋`esp_cache_msync`）→ 安裝完成中斷
  - [ ] `submit(slot)`：PSRAM 槽先 `esp_cache_msync(C2M)`；唔好喺 ISR 之外重入
  - [ ] ISR：`done_order` 機制**照抄現有 `nc_i80_drv.c`**（order 對槽，不假設 FIFO）
  - [ ] log 一行：`LCD_CAM 直驅：lanes=1 8-bit pclk=2500000 Hz、一筆 X B、3 槽（無 WR／DC）`
- [ ] `CMakeLists.txt` 二選一：

```cmake
if(CONFIG_NETCORE_PIXEL_I80_DIRECT)
    set(_i80_srcs "nc_i80_drv_direct.c")
else()
    set(_i80_srcs "nc_i80_drv.c")
endif()
idf_component_register(SRCS ${_i80_srcs} INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_lcd esp_driver_gpio heap log)
```

- [ ] 建置 rc=0；**燒之前**先 `--no-apply` 驗 image 好唔好（可選）
- [ ] Commit：`feat(drv_i80): 直接寫 LCD_CAM 的驅動（同介面、只 route lane）`

## 5. Task 4：上層接駁（30 分鐘）

- [ ] `nc_i80_out.c`：

```c
const nc_i80_drv_cfg_t dc = {
    .pins = CONFIG_NETCORE_PIXEL_I80_DIRECT ? NULL : cfg->pins,
    .lane_pins = cfg->lane_pins,          // 新增（cfg 內已有）
    .lanes = cfg->lanes,
    .pclk_hz = cc.wave.clk_hz,
    .slot_bytes = d->units * (uint32_t)(d->bus_width / 8),
    .slots = cfg->slots,
    .bus_width = d->bus_width,
};
```

- [ ] `nc_hw_claim()` 只在 direct 版 claim `lane_pins[0..lanes-1]`＋（esp_lcd 版照舊 claim
      `pins[0..width+2-1]`）——保留腳／重複檢查仍然要全部做一次（lane 腳）
- [ ] `nc_render_task.c` 的 `i80_src()`：direct 版輸出 `lane_pins`（= `hw->i80[b].data[0..lanes-1]`）
      同 `lanes`；esp_lcd 版照舊出 `pins`（data＋WR＋DC）
- [ ] 建置 rc=0 + host test 21/21
- [ ] Commit：`feat(render): I80 adapter 支援 direct 驅動（只傳 lane 腳）`

## 6. Task 5：上板驗證（硬件閘門，唔可以跳）

- [ ] `tools/ota.py --port COM24 artifacts/ESP32-S3-Matrix__ESP32-S3-Matrix-LED-Textkit.bin`
- [ ] `tools/nc4.py --port COM24 status sys` → `rst=3`、`idf=v6.1.1`
- [ ] `tools/nc4.py --port COM24 status tasks` → 四任務 `level=4`、`started=1`、`running=1`
- [ ] `tools/nc4.py --port COM24 status mem` → 環 slot 320 B × 3（同之前一樣）
- [ ] 讀 log（WebMaster `/api/log/tail` 或 `nc4.py listen`）→ 見到
      `LCD_CAM 直驅：lanes=1 8-bit pclk=2500000 …`
- [ ] `0xF071`（用 `artifacts/_tmp/px_live.py` 或 WebMaster）→ **≥49 fps、`drops=0`**、
      `tx_us` 唔應該比 esp_lcd 版差太多（記錄 A/B）
- [ ] `0xF06B` 切 `rainbow` → **64 顆 64 種顏色**；`white_breath` → 同色漸暗
- [ ] **肉眼**：8×8 正常、唔閃、顏色次序對（GRB）
- [ ] 回歸：`0xF06D` 切三個 mode 都正常；reset 後仍正常
- [ ] 記錄：image sha256、`fw`、`app`、idf、`0xF071` 數字（放 status §7.254）

**失敗回退**：`CONFIG_NETCORE_PIXEL_I80_DIRECT=n` 重 build（一分鐘）即可回到 esp_lcd 版；
記錄失敗症狀（顏色錯／閃／重建不起）再修，唔好硬推。

## 7. Task 6：階段②（config 稀疏化，driver 通了才做）

- [ ] `nc_cfg_load.c apply_i80()`：`-1`＝未用、省略尾部、`n_used`、`bus_width = lanes>8?16:8`、
      `wr`/`dc` 可省（direct 版根本唔用；esp_lcd 版仍然要 → 由池補）
- [ ] `nc_i80_cfg_t` 加 `n_used`；`nc_i80_out.c`／`nc_render_task.c` 用 `n_used`
- [ ] host test：`tools/hosttest/cfg_test` 新 case（`[14]`／`[14,-1…]`／`[]`／`wr` 省／16-bit 邊界）
- [ ] 上板：live `hardware.json` 改成 `"data":[14]`（無 `wr`／`dc`）→ reboot → 正常出燈
- [ ] 文件：設計稿 §9 五條問題的結論寫實、`configuration.md` 更新
- [ ] Commit：`feat(config): I80 data 稀疏化（-1＝未用、自動 8↔16-bit）`

## 8. Definition of Done

- [ ] host test 21/21
- [ ] 建置 rc=0（兩個 Kconfig 版本都 build 得）
- [ ] 上板：四任務 running、50 fps、`drops=0`、`0xF06B` 正確、肉眼正常
- [ ] Matrix 的 dummy 腳（38–42／47／48／1／2）**唔再 claim／唔再需要寫**
- [ ] `status.md` 新一節 + 設計稿更新 + AGENTS §10.4 一行
- [ ] 本地 commit（**未 push**）

---

## 附錄 A：IDF 6.1.1 LCD_CAM i80 底層要點（已對過原始碼）

> 主要參考：`/home/user/esp/esp-idf/components/esp_lcd/i80/esp_lcd_panel_io_i80.c`（935 行；
> WSL 路徑）。**不要抄整份**，只抄「建立 bus＋送一筆 DMA」的最小序列。

### A1. 時鐘（`esp_lcd_new_i80_bus` L122-166、per-device L357-379、L817）

```c
lcd_ll_enable_bus_clock(bus_id, true);
lcd_ll_reset_register(bus_id);
lcd_ll_select_core_clk_src(bus_id, LCD_CORE_CLK_SRC_DEFAULT);
lcd_ll_set_core_clock_divider(bus_id, 2, 0, 0);      // 先當固定
// per transaction/device：
uint32_t prescale = bus->resolution_hz / 2500000;     // resolution_hz = 選定 clk source 的頻率
ESP_GOTO_ON_FALSE(prescale > 0 && prescale <= LCD_LL_PCLK_DIV_MAX, ...);
lcd_ll_set_pixel_clock_prescale(dev, prescale);
lcd_ll_set_clock_idle_level(dev, !pclk_idle_low);     // 我們要 idle low
lcd_ll_set_pixel_clock_edge(dev, pclk_active_neg);    // 我們要 active low=0 → false
```

- `pclk_hz` 我們固定 2.5 MHz（同現時 `driver.json` 的 `timing.pclk_hz`）。
- `resolution_hz` 由 `bus->hal`／clk source 決定，IDF 用 `lcd_hal_get_clock_resolution()`；
  實作時照抄 L350-360 那段就算。

### A2. i80 模式（bus 初始化 L225-236、L769、L795-796）

```c
lcd_ll_enable_rgb_mode(dev, false);          // i80 模式
lcd_ll_enable_color_convert(dev, false);
lcd_ll_set_dma_read_stride(dev, width);      // 8 或 16（bytes per bus word）
lcd_ll_set_swizzle_mode(dev, LCD_LL_SWIZZLE_AB2BA);
lcd_ll_enable_output_always_on(dev, true);
lcd_ll_set_data_wire_width(dev, width);
lcd_ll_reset(dev); lcd_ll_fifo_reset(dev);
lcd_ll_set_phase_cycles(dev, 0, 1, 0);       // 對「只有 data」的 transaction
lcd_ll_set_blank_cycles(dev, 1, 1);
```

> 我們**沒有 cmd phase**（現在 esp_lcd 版是 `lcd_cmd=-1`），所以 cmd_cycles=0，
> `data_cycles = color_size / (width/8)`。`lcd_ll_set_command()` 唔使叫。

### A3. GPIO route（`lcd_i80_bus_configure_gpio` L735-762）

```c
for (i = 0; i < lanes; i++) {                        // ← 關鍵：只 loop 真正 lane！
    gpio_func_sel(lane_pins[i], PIN_FUNC_GPIO);
    esp_rom_gpio_connect_out_signal(lane_pins[i],
        soc_lcd_i80_signals[bus_id].data_sigs[i], false, false);
}
// dc/wr 唔 route（i80 的 WR 唔接都得；FastLED 在 S3 就是咁）
```

- `soc_lcd_i80_signals` 由 `hal/lcd_ll.h`／`hal/lcd_hal.h` 一帶引入（IDF 檔本身冇直接
  include 它，係 transitively 拿到；實作時用同一個 include 組合）。
- IDF 的 `valid_gpio` 檢查（L739-743）只在「齊唔齊」時決定要唔要 route；我們**直接跳過
  DC／WR**，只需要 lane 腳有效、唔重複、非保留腳。

### A4. DMA（`lcd_i80_init_dma_link` L46／L122-222、tx_color L578-620、L785-833）

IDF 用 **GDMA channel＋link list**，一筆 transaction 一個 descriptor：

```c
gdma_new_ahb_channel(...)  // 或 axi（P4）；S3 用 ahb
gdma_link_list_create(...) // 或 gdma_new_link_list
gdma_get_buffer_alignment_constraint(chan, buf)
gdma_buffer_mount_config_t mc = { .buffer=buf, .buffer_alignment=align,
    .length=bytes, .flags={ .mark_eof=true, .mark_final=GDMA_FINAL_LINK_TO_NULL } };
gdma_link_mount_buffers(link, 0, &mc, 1, NULL);
lcd_start_transaction(bus, trans_desc);   // L785-833：set phase cycles + gdma start
```

- **階段①可以簡化**：同一時間只跑一筆（64 px × 24 bit × 3 cells = 4,608 tick @2.5 MHz
  ＝ **1.84 ms** 線路時間，而幀距 20 ms），所以「submit → 開 DMA → 完成中斷 → 下一筆」
  就夠，唔需要 IDF 的 queue／`done_queue` 兩層。
- 但要保留我們現有的 **`done_order` 對槽機制**（槽唔係 FIFO）。

### A5. 完成中斷（L212-219、L527-534、L834-880）

```c
esp_intr_alloc_intrstatus(irq_id, ESP_INTR_FLAG_IRAM,
    (uint32_t)lcd_ll_get_interrupt_status_reg(dev),
    LCD_LL_EVENT_TRANS_DONE, isr, ctx, &intr);
// ISR：
uint32_t st = lcd_ll_get_interrupt_status(dev);
lcd_ll_clear_interrupt_status(dev, st);
if (st & LCD_LL_EVENT_TRANS_DONE) { /* 標記這一筆完成 + 開下一筆 */ }
```

- ISR 要 `IRAM_ATTR`；入面**唔可以** log 大字串（鐵律 8 同理）。
- IDF 在 ISR 內 `esp_intr_disable()`，有下一筆才 `lcd_ll_enable_interrupt()+esp_intr_enable()`。

### A6. Cache（tx_color L585-588）

```c
if (esp_cache_get_line_size_by_addr(color) > 0) {
    esp_cache_msync((void *)color, size,
        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}
```

- 我們現時的 PSRAM 槽靠 esp_lcd 做；**直接驅動要自己叫這兩行**（internal 槽會回 0，唔會做）。
- 64 px 的 Matrix 槽是 internal（≤48 KB），所以階段①其實唔會踩到 cache 路；但仍要寫對。

### A7. 風險清單（實作時逐項打勾）

- [ ] 8-bit 模式 `lcd_ll_set_dma_read_stride(dev, 8)`＋`set_data_wire_width(dev, 8)` 同時做
- [ ] `phase_cycles(0,1,0)` 對應「只有 data phase」，`data_cycles > 0`
- [ ] 完成中斷有冇 `LCD_LL_EVENT_UNDERRUN` 情況（DMA 追唔上）→ 要有 count
- [ ] PSRAM 槽 msync
- [ ] `bus_id`：S3 只有 0（`soc_lcd_i80_signals[0]`）
- [ ] 如果 2.5 MHz prescale 除唔盡（resolution/2.5M 有餘數）→ 先試最近值，log 實際 PCLK

