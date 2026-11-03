# esp_Net-Core — C / ESP-IDF 重寫計劃

> **狀態（2026-11-03 更新）**：Phase 0／1 大部分已落地 —— NC4 通道（CDC／UART／TCP／WS／UDP）、
> 檔案查詢、FAT 擁有者（MSC 只做 recovery）、任務管理器、`tasks.json`、`.fxe` 掃描都已在板上跑過。
> Phase 2（pixel pipeline）與 Phase 3（cross-chip sync）**還沒開始**。
>
> ⚠️ **近期要做什麼請看 [status.md §7](status.md)，不要照這份的 Phase 順序排** ——
> 這份留的是產品級分期（燈效／同步／recovery），不是當前衝刺清單。
>
> 原則：**一個 command layer（NC4）、一個 FS owner（device）、一個 frame clock**
> 架構圖：`docs/03_notes/architecture-flow.md`

---

## 0. 背景與目標

本 repo 是 Net-Core 的 C / ESP-IDF 版本。既有 MicroPython 版作為行為參考（protocol、asset schema、錯誤碼），不是逐行 port。

C 版的目標不是「math 快 50%」，而是：

- **Deterministic overlap**：compute N+1 與 output N 重疊（DMA + double buffer）。
- **Lower jitter**：`esp_timer`／ISR 取代 Python task 調度。
- **Parallel output**：RMT／I2S／LCD_CAM 多線並行。
- **Cross-chip sync**：common timebase + scheduled start + frame id。
- **Single frame contract**：effects 只 render 到 buffer，不准碰硬體。

---

## 1. 已定案

| 項目 | 決定 |
|---|---|
| 編輯路徑 | **NC4 over UART / Wi-Fi / CDC**；不做獨立 CDC file protocol |
| FS owner | **Device**；host 不 mount FAT（除 recovery） |
| Assets | boot／reload 載入 RAM；frame 期間不讀 FS |
| 寫入 | staging → CRC → validate → atomic rename → `ASSET_RELOAD` |
| MSC | **只做 recovery**，不是日常編輯 |
| Pixel | FrameClock + RenderTask + OutputTask + mailbox + DMA |
| Effects | `render(frame_id, time_us, span)`；不准 call `show()` |
| Sync | master timebase + scheduled start；µs timestamp |
| CDC | log／control transport；不是 file protocol |

---

## 2. 非目標

- 不做 concurrent FAT：MSC 只做 recovery。
- 不追求把 math 改成 C 來換 50%；math 的損失可接受。
- 不在 hot path 讀 JSON／FS。
- 不把 FastLED 的 `show()` 模式帶過來。

---

## 3. 分期

### Phase 0 — Contract freeze ✅（已完成；結論與原計劃不同）

> **實際結果（不要把下面四條當成現況）**：
> - **沒有做** `FILE_BEGIN` / `FILE_CHUNK` / `FILE_END` / `BUNDLE_*`。裝置端只有
>   `FILE_QUERY`（`0x2005`）＋ `STATUS_SET`（`0x1103`）子集；寫檔走 MSC 或 HTTP `PUT`。
>   見 [commands.md](../01_protocol/commands.md)。
> - **`ASSET_RELOAD` 還沒做**（提案 `0x1019`）—— 現在改完檔案要重開機。
> - FS 決定 **FAT**（MSC recovery 要能直接讀），見 [fat-ownership.md](../03_notes/fat-ownership.md)。
> - golden case 沒有獨立 vector 檔，用 `tools/hosttest/` 與 `tools/bench/` 的實測代替。

<details>
<summary>原計劃（保留作記錄）</summary>

- 凍結 NC4 file commands：`FILE_BEGIN`、`FILE_CHUNK`、`FILE_END`、`FILE_ABORT`、`BUNDLE_*`、`ASSET_RELOAD`。
- 凍結 asset schema：effects／map／modes／registry；錯誤碼。
- 由既有 MicroPython 版 capture golden cases：正常、壞 CRC、半寫入、dangling ref、power-loss。
- 決定 FS：LittleFS（primary）或 FAT（MSC recovery 需要）。

**交付**：`docs/nc4_file_protocol.md` + golden test vectors。

</details>

### Phase 1 — C/IDF Foundation 🟡（大部分完成）

> 已落地：IDF 工程骨架（boot／NVS／FAT／WL／logging）、NC4 通道（CDC／UART／TCP／WS／UDP）、
> 檔案查詢與 HTTP 設定讀寫、任務管理器、`tasks.json`、`.fxe` 掃描。
> **未做**：asset loader（JSON schema 驗證）、`ASSET_RELOAD`、bundle commit。
> 驗收項裡的「斷電 20 次」沒有正式跑過。

<details>
<summary>原計劃（保留作記錄）</summary>

- ESP-IDF project skeleton：boot、NVS、partition、FS、logging。
- NC4 transport：UART + Wi-Fi（TCP／WS）；CDC 可選 log。
- File service：staging、CRC、atomic rename、bundle commit。
- Asset loader：JSON parse、schema／ref validation、RAM 版本、`ASSET_RELOAD`。
- 暫不接 pixel。

**驗收**：

- 用 NC4 上傳一組 assets，commit 後 `ASSET_RELOAD` 成功。
- 壞 CRC／壞 JSON／dangling ref → 保留舊版，回錯誤。
- 斷電測試 20 次：要麼舊版完整，要麼新版完整，不能半寫入。
- 無 pixel 時 CPU idle 正常；無 FS mount 泄漏。

</details>

### Phase 2 — Pixel Pipeline（2–4 週）⬜ 未開始

- `FrameClock`：`esp_timer` 固定 period（例：20 ms）。
- `RenderTask`：effects → back buffer；compute N+1 與 output N 重疊。
- Mailbox：2 slot + seq，latest-frame，不是深 FIFO。
- OutputTask／drivers：
  - WS2812：RMT TX + DMA + `on_trans_done`
  - APA102：SPI DMA
  - PCA9685：I2C（只寫 dirty）
  - UART motor：TxCenter queue + coalesce
- `TxCenter`：共享 UART 的 single writer、priority、metrics。
- Effects API：`render(frame_id, time_us, span)`；hardware 只在 output task。

**驗收**：

- 676 px 目標 50 FPS：p95 frame time ≤ 20 ms，1 小時 soak 無 drop。
- Miss deadline：重播上一幀，`dropped` counter 增加，frame clock 不漂移。
- CPU load、IRAM／DRAM、flash write stall 有量測數據。

### Phase 3 — Cross-chip Sync（1–2 週）⬜ 未開始

- `esp_timer_get_time()` µs timebase。
- Master broadcast `T0_us` + `period_us`；slave schedule `T0 + n*period`。
- Offset 用 min-RTT 估；每 1–5 秒 resync。
- Frame id + `output_start_us` telemetry。
- <1 ms 需求：RS485／GPIO sync pulse，不靠 Wi-Fi。

**驗收**：

- 有線：相鄰 node frame start delta < 1 ms（目標）。
- Wi-Fi：< 5 ms（目標，視環境）。
- 1 小時 drift < 1 frame。

### Phase 4 — Recovery MSC（optional，3–5 天）

- BOOT button／NVS flag 進入 recovery。
- 停止 output；firmware 不 mount FAT；USB MSC + CDC。
- Host 編輯；eject → `RUN` command → NVS RUN → reboot。
- `last_good` + manifest hash；validate fail → 留在 recovery。

**驗收**：

- Assets 壞掉 → 可進 recovery，host 可修復。
- 正常 RUN 時 MSC 完全不出現。

### Phase 5 — Migration（持續）

- MicroPython 版與 C 版並行；NC4 協議兼容。
- A/B 測試：同一組 assets、同一組指令，比較 frame time／FPS／sync。
- 保留 rollback firmware。

---

## 4. 開放決策

> **1 與 2 已經定了**（FAT、MSC 只做 recovery），3–5 仍然開放。

1. ~~**FS**：LittleFS vs FAT~~ → **定案 FAT**（MSC recovery 要能直接讀），見 [fat-ownership.md](../03_notes/fat-ownership.md)。
2. ~~**MSC recovery**：要做嗎~~ → **只做 recovery，不是日常編輯**（同上）。Phase 4 保留。
3. **CDC**：只做 log，還是也做 NC4 transport？若做，ViperIDE 需要 native transport。
4. **Target**：每 node max px、target FPS、sync tolerance（例：676 px／50 FPS／±1 ms）。
5. **Hardware**：S3／P4；USB OTG pins；VBUS detect 有沒有。

---

## 5. 第一批任務 ✅（已完成）

> 已完成到「可以上板跑」的程度，與原定內容的差異：
> 1. ~~`docs/nc4_file_protocol.md`~~ → 沒寫；協議現況集中在 [commands.md](../01_protocol/commands.md)。
> 2. ~~golden tests~~ → 用 `tools/hosttest/`（hub／plan／fx／fxm／task／cfg）代替，沒有獨立 vector 檔。
> 3. ✅ ESP-IDF skeleton + FS + NC4 UART（另加 CDC／TCP／WS／UDP）。
> 4. ✅ `README.md` 與 `.gitignore`。
>
> **下一步不在這裡** —— 看 [status.md §7](status.md)。
