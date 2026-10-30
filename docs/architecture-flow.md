# esp_Net-Core（C / ESP-IDF）流程概念圖

> 本文件描述 C / ESP-IDF 版本的目標架構；既有 MicroPython 版為行為參考。
> 設計原則：
> 1. **一個 command layer（NC4），多個 transport（UART / Wi-Fi / CDC）**。
> 2. **Device 擁有 FS**；NC4 file service 寫 staging → validate → atomic swap。
> 3. **RUN 時不開 MSC**；MSC 只做 recovery。
> 4. **Pixel 只讀 RAM assets**，hot path 不碰 FS。
> 5. **Frame clock 是唯一輸出節拍**；effects 不可以直接碰硬體。

---

## 1. 系統總覽

```text
┌────────────────────────────── HOST ──────────────────────────────┐
│ Master / PC tool / ViperIDE (native NC4)                         │
│   control : MODE_SET, TIME_SYNC, STATUS, ...                     │
│   file    : FILE_BEGIN, FILE_CHUNK, FILE_END, BUNDLE_*           │
└───────────────┬───────────────────────────────┬──────────────────┘
                │ UART / RS485                  │ Wi-Fi TCP / WS
                │ (CDC 亦可，同一 NC4 framing)   │
                ▼                               ▼
        ┌──────────────────────────────────────────────┐
        │ Transport adapters                           │
        │ CircuitBus / NetBus / CDC                    │
        └───────────────────┬──────────────────────────┘
                            ▼
        ┌──────────────────────────────────────────────┐
        │ NC4 command layer                            │
        │ schema → dispatch → action                   │
        │  ├─ control (mode / time / status)           │
        │  └─ file service (device owns FS)            │
        └───────────┬──────────────────────┬───────────┘
                    │                      │
                    ▼                      ▼
        ┌───────────────────┐   ┌──────────────────────┐
        │ Asset store       │   │ Pixel pipeline       │
        │ staging → commit  │   │ compute → mailbox    │
        │ current/last_good │   │ → render → drivers   │
        └───────────────────┘   └──────────────────────┘
```

重點：

- CDC 不是獨立 file protocol；它只是 NC4 的另一條 transport。
- FS 只有 device 一個 owner；host 不會 mount FAT（除了 recovery）。
- Pixel 的 assets 在 boot／reload 時載入 RAM；frame 期間不讀 FS。

---

## 2. Boot / Mode 決策

```text
Power on
  │
  ├─ NVS next_mode? ── yes ──> use once（清除 flag）
  │
  ├─ BOOT button held? ── yes ──> RECOVERY
  │
  ├─ assets valid? ── no ──> RECOVERY
  │
  └─ RUN
       ├─ mount FS
       ├─ load JSON assets → RAM
       ├─ validate refs / schema
       ├─ unmount FS
       ├─ start pixel pipeline（FrameClock / Render / Output）
       └─ start NC4 transports（UART / Wi-Fi；CDC optional log）

RECOVERY
  ├─ stop output / motors neutral
  ├─ do NOT mount FAT（host 獨佔）
  ├─ USB MSC + CDC
  ├─ host edits files
  └─ exit: eject → RUN command → NVS RUN → reboot
```

| 模式 | USB | FS owner | Output |
|---|---|---|---|
| RUN | CDC only（optional） | device | 全速 |
| RECOVERY | CDC + MSC | host | 停止 |

---

## 3. NC4 檔案編輯 / Commit

```text
Host                                Device (NC4 file service)
 │ FILE_BEGIN(name, size, crc)      │
 ├─────────────────────────────────>│ open staging file
 │ FILE_CHUNK(offset, data)         │
 ├─────────────────────────────────>│ write + update crc
 │ ...                              │
 │ FILE_END                         │
 ├─────────────────────────────────>│ verify crc
 │                                  │ validate JSON / bundle refs
 │                                  │ atomic rename staging → current
 │                                  │ asset_version++
 │                                  │ ASSET_RELOAD
 │<─────────────────────────────────┤ OK / ERR
 │                                  │ pixel swaps at next frame boundary
```

- 多檔一起改：`BUNDLE_BEGIN` → 全部寫 staging → `BUNDLE_COMMIT` → validate 全部 refs → swap。
- Fail：保留 `current` / `last_good`，回傳錯誤，不半寫入。
- Flash write 會 stall 20–50 ms；可以暫停 output 一兩幀，或接受一次 glitch。

---

## 4. Runtime Frame Pipeline

```text
Core 1 (compute)                 Core 0 (render / output)
┌────────────────┐   mailbox   ┌─────────────────────────┐
│ PixelTask      │ ──────────> │ RenderTask              │
│ effects        │  latest     │ read latest frame       │
│ scatter        │  frame      │ show_all / TxCenter     │
└────────────────┘             └───────────┬─────────────┘
                                           │
                             ┌─────────────┼─────────────┐
                             ▼             ▼             ▼
                          WS2812 RMT    APA102 SPI    PCA / UART
```

- Mailbox 是 **latest-frame**（2 slot + seq），不是深 FIFO；舊 frame 直接丟。
- `FrameClock` 用 `esp_timer` 固定節拍；miss deadline 就重播上一幀，不推遲節拍。
- `TxCenter` 只負責共享 bus（UART motor / NC4）的仲裁、priority、coalesce。
- Effects API：`render(frame_id, time_us, span)`；**不准 call show()**。
- Core 分配是目標；實作時用 `xTaskCreatePinnedToCore` 明確 pin，不要靠預設。

---

## 5. Ownership Matrix

| 資源 | RUN | RECOVERY |
|---|---|---|
| FAT / assets partition | device mount → load → unmount | host mount（MSC） |
| MSC | off | on |
| CDC | optional log/control | on |
| Pixel output | on | off |
| NC4 file service | on（device FS） | 不使用（host 直接改） |
| Flash write | 只在 commit 時 | host 寫入時 |

---

## 6. 一句總結

```text
Host tools
   │  NC4 over UART / Wi-Fi / CDC
   ▼
NC4 command layer
   ├─ control
   └─ file service → staging → validate → atomic swap → asset_version
   ▼
RAM assets
   ▼
Pixel pipeline: compute → mailbox → render → RMT/SPI/I2C/UART
```

**一個 command layer、一個 FS owner、一個 frame clock。**
