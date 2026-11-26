# LCD_CAM 直接驅動（階段① **已完成 2026-11-26**）＋ 稀疏 I80 schema（階段②）實作計劃

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

## 附錄 A：IDF 6.1.1 LCD_CAM i80 底層要點（v2：已由 subagent 對原始碼核實）

> 來源：`$IDF`=repo `.idf/esp-idf`（v6.1.1）、`$HL`=`components/esp_hal_lcd`、
> `$E`=`components/esp_lcd`（主要係 `$E/i80/esp_lcd_panel_io_i80.c`）。
> **S3 冇 LCD 專用 DMA**：LCD_CAM 只有 16-deep FIFO（`$HL/esp32s3/include/hal/lcd_ll.h:41`），
> 資料一定要由 **AHB GDMA（trigger id 5）** 餵。

### A0. 三個命名校正（同舊文／TRM 唔一致）

1. `LCD_CAM.i80_ctrl` 在 6.1.1 **已改名** `LCD_CAM.lcd_user`（`soc/lcd_cam_reg.h:422-425`）：
   `lcd_8bits_order`(bit19)、`lcd_dout_cyclelen`[12:0]、`lcd_2byte_en`(bit23)、
   `lcd_start`(bit27)、`lcd_always_out_en`(bit13)。
2. **冇 `hal/lcd_cam_ll.h`**；S3 的 LL 是 `$HL/esp32s3/include/hal/lcd_ll.h`（component
   `esp_hal_lcd`，include 係 public）。`LCD_LL_GET_HW(0) == &LCD_CAM`。
3. 6.1.1 的 i80 **唔再用 `esp_async_memcpy`**（M2M 冇 peripheral trigger）；用 AHB GDMA。

### A1. 時鐘：2.5 MHz PCLK（精確、零 jitter）

| 步 | 動作 |
|---|---|
| a | `PERIPH_RCC_ACQUIRE_ATOMIC(PERIPH_LCD_CAM_MODULE, rc){ if(rc==0){ lcd_ll_enable_bus_clock(0,true); lcd_ll_reset_register(0);} }`（i80.c:157-167） |
| b | `lcd_ll_enable_clock(dev,true)`（i80.c:195-197） |
| c | `esp_clk_tree_enable_src(LCD_CLK_SRC_DEFAULT,true)` + `lcd_ll_select_clk_src(0,src)`；**S3 = PLL_F160M 160 MHz**（i80.c:199-205,652-661） |
| d | `lcd_ll_set_group_clock_coeff(0, LCD_PERIPH_CLOCK_PRE_SCALE=2, 0, 0)` → **LCD_CLK = 80 MHz**（i80.c:662-667） |
| e | `prescale = resolution_hz / pclk_hz`（**整數 1..64**）→ `lcd_ll_set_pixel_clock_prescale(dev,N)`；**2.5 MHz = 80/32 → N=32**（i80.c:358-360,378-379,817） |
| f | 選完 clk src 才 `lcd_ll_reset()` + `lcd_ll_fifo_reset()`（i80.c:206-208） |

- 唔用分數除頻（`lcd_clkm_div_a/b`）避免 jitter（i80.c:662-663）；2.5 MHz 整除，唔需要。
- ⚠ 1 MHz 會 `ESP_ERR_NOT_SUPPORTED`（80/1=80>64）——所以 prescale 要檢查。

### A2. i80 bus 寄存器

- **8/16-bit**：`lcd_ll_set_dma_read_stride(dev,width)` → 8 ⇒ `lcd_2byte_en=0`（1 B/PCLK）；
  16 ⇒ `=1`。S3 的 `lcd_ll_set_data_wire_width()` 係 **no-op**（i80.c:229,747）。
- **只有 DOUT phase**：`lcd_ll_set_phase_cycles(dev,0,0,1)` + `lcd_ll_enable_output_always_on(dev,true)`
  → `lcd_dout=1`、`lcd_dout_cyclelen=0`，**有幾多 word 由 DMA buffer／EOF 決定**（i80.c:232-233,790-791）。
- **位元組序全 0**：`lcd_8bits_order=0`、`lcd_bit_order=0`、`lcd_byte_order=0`（i80.c:231,534,576,896）。
- `lcd_ll_enable_rgb_mode(false)`、`lcd_ll_enable_color_convert(false)`。
- **DC／CS 可以完全唔要**（唔 route 就冇 pin）；DC level 由 `lcd_ll_set_dc_level()` 控制。
- blank cycles：esp_lcd 每筆 `lcd_ll_set_blank_cycles(dev,1,1)`（前後各多 1 PCLK）；
  decoder driver 可以試 (0,0) 慳 2 PCLK，但要先確認最後一個 word 的 hold。

### A3. GPIO matrix（只 route lane + PCLK）

- 訊號 index（`soc/gpio_sig_map.h:234-276`）：`LCD_DATA_OUT0..15` = **133..148**、
  `LCD_PCLK_IDX` = **154**（WR 同 PCLK 同一個）、`LCD_DC_IDX`=153、`LCD_CS_IDX`=132。
- 官方 mapping：`$HL/esp32s3/lcd_periph.c:10-34`（`soc_lcd_i80_signals[0]`，`irq_id=ETS_LCD_CAM_INTR_SOURCE`）。
- Route：`gpio_func_sel(pin,PIN_FUNC_GPIO)` + `esp_rom_gpio_connect_out_signal(pin,sig,false,false)`（i80.c:750-760）。
- **我們只 loop `lanes` 次**（data_sigs[i]），唔 route DC／WR → 就係「零 dummy」的關鍵。

### A4. DMA（AHB GDMA + link list）

1. `gdma_new_ahb_channel(&cfg,&tx,NULL)`（i80.c:686）；S3 trigger =
   `SOC_GDMA_TRIG_PERIPH_LCD0`=5、bus=AHB。
2. `gdma_connect(chan, GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_LCD,0))`（i80.c:688）。
3. `gdma_apply_strategy({auto_update_desc=true, owner_check=true})`（i80.c:689-693）。
4. `gdma_config_transfer({max_data_burst_size=32, access_ext_mem=true})`（i80.c:695-699）。
5. Link list：`gdma_new_link_list({num_items=esp_dma_calculate_node_count(...), item_alignment=4})`
   （單 node 上限 4095 B）；buffer 對齊 `gdma_get_buffer_alignment_constraint()`。
6. **發動一筆**（i80.c:785-809）：phase/blank → `lcd_ll_reset`+`fifo_reset` →
   `gdma_link_mount_buffers(list,0,{buf,len,mark_eof=true,mark_final=GDMA_FINAL_LINK_TO_NULL},1,NULL)`
   → `gdma_start(chan, head_addr)` → `esp_rom_delay_us(4)` → `lcd_ll_start(dev)`。
7. ⚠ **GDMA EOF ≠ wire 出完**（FIFO 仲可以有 ≤16 word）——「還格」要用 LCD `trans_done`
   （即 A5），唔可以靠 GDMA EOF。

### A5. 完成中斷（LCD_CAM 自己，唔係 GDMA）

- `ETS_LCD_CAM_INTR_SOURCE = 24`（**level**，同 camera 共用）。
- `esp_intr_alloc_intrstatus(24, DISABLED|SHARED|LOWMED, (uint32_t)lcd_ll_get_interrupt_status_reg(dev),
  LCD_LL_EVENT_I80, isr, arg, &intr)`（i80.c:211-215）；status = `lc_dma_int_st` bit1。
- `lc_dma_int_st` 係 **masked**（ena=0 ⇒ 讀到 0），所以 CPU 側用 `esp_intr_enable/disable()` gating。
- ISR：讀 status → 標記完成／開下一筆 → `lcd_ll_clear_interrupt_status(dev, LCD_LL_EVENT_TRANS_DONE)`
  （寫 `lc_dma_int_clr`）。level 唔 clear 會 storm。

### A6. Cache（S3）

- PSRAM buffer：submit 前一定要
  `esp_cache_msync(buf,len, ESP_CACHE_MSYNC_FLAG_DIR_C2M|ESP_CACHE_MSYNC_FLAG_UNALIGNED)`，
  先 `esp_cache_get_line_size_by_addr(buf)>0` 才做（i80.c:585-588）。
- **internal SRAM 唔經 cache**（`SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE` 未定義）→ 64 px 的
  Matrix 槽（internal）唔使做；但 code 要寫對，因為 16×600 那類會用 PSRAM。

### A7. LL API（可直接用，include `hal/lcd_ll.h`）

`lcd_ll_enable_bus_clock`／`reset_register`／`enable_clock`／`select_clk_src`／
`set_group_clock_coeff`／`set_pixel_clock_prescale`／`set_clock_idle_level`／
`set_pixel_clock_edge`／`set_phase_cycles`／`set_blank_cycles`／`set_dma_read_stride`／
`enable_output_always_on`／`start`／`stop`／`reset`／`fifo_reset`／`enable_rgb_mode`／
`set_dc_level`／`enable_interrupt`／`get_interrupt_status`／`clear_interrupt_status`／
`get_interrupt_status_reg`。

**Component 需要**：`REQUIRES esp_hal_lcd esp_driver_dma soc hal esp_hw_support`
（`esp_hal_lcd` include 係 public；`esp_private/gdma.h` 屬 private 但可用 API）。

### A8. 可以簡化（esp_lcd 有、我們唔需要）

唔使 bus slot 仲裁、DC／CS、`lcd_cmd_bits`/`lcd_param_bits`、cmd/dummy phase、
format buffer／byte 反轉、multi-device switch、PM lock、sleep retention、
trans/done queue；PCLK 設定一次就夠。

### A9. 三個陷阱

1. `lcd_always_out_en=1` 之後 word 數只由 DMA 決定；`lcd_dout_cyclelen=0` 唔係「0 個 cycle」。
2. `lcd_ll_start()` 前必須 `lcd_update=1`（LL 已代做）；改 prescale／phase 後要再 update。
3. 中斷 level+masked：`st` 讀到 0 唔代表冇事件；clear 一定要寫 `lc_dma_int_clr`。


