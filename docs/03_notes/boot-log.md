# 開機 log 的兩個出口：console 與管道

> 一句：**log 由 B01 就開始產生；「看不看得見」取決於你看哪一個出口。
> B10 是「管道出口」開啟，不是第一行 log。**
>
> 相關：[usb-config.md](usb-config.md)（B10 的 USB）、
> [config-layers.md](config-layers.md)（`log_debug_mode`）、
> 程式：[nclog](../../firmware/components/sys/nclog/nc_log.c)、[main.c](../../firmware/main/main.c)

---

## 1. 兩個出口

| 出口 | 是什麼 | 何時可用 | 誰在寫 |
|---|---|---|---|
| **A. IDF console** | ESP-IDF 的 `vprintf` → UART0／USB-Serial-JTAG | **通電就有**（ROM／IDF） | IDF ＋ 我們的 `ESP_LOG` |
| **B. nc_log 管道** | hook → RAM 環 → `nc_log` task → USB（CDC／USJ） | **B10 `nc_log_start()`** | nclog |

B01 的 `nc_log_capture()` 只做兩件事：讀級別、配環。**管道未開**（沒有 consumer），
所以 B01–B09 經管道看不到東西 —— 那些行走的是出口 A。

### 1.1 為什麼「未初始化」都出得去？

因為「未初始化」講的是 **TinyUSB／OTG**（我們的 NC4 USB 資料通道），
不是 IDF console。console 這條路 **在 `app_main` 之前就已經由 bootloader／IDF 設好**：

| console | 誰設定 | 怎麼寫出去 | 要我們的緩衝嗎 |
|---|---|---|---|
| **UART0** | bootloader 設腳位／baud；IDF 註冊 VFS | 直接寫 UART TX FIFO；FIFO 滿就等 | 不要 |
| **USB-Serial-JTAG** | ROM／IDF（USJ 本身） | 逐 byte 寫 USJ 的 LL TX FIFO（幾十 B）；滿了最多等 50 ms，冇 host 就 drop | 不要 |

- IDF 在 startup 時就註冊 console（`esp_stdio`：`ESP_SYSTEM_INIT_FN(init_vfs_console)`），
  所以 `ESP_LOG` 由第一行就有路可走。
- USJ console **不需要 TinyUSB、不需要我們 install 任何 driver**：它直接寫硬體 FIFO。
  所謂「沒有緩衝」只是「沒有**我們**的大緩衝」——硬體 FIFO 本身就是那條細管道。
- 我們的大緩衝（`usb.rx` 4,128 B、nc_log 環 2／8 KB）是 **B01／B07 才由我們配置**，
  只服務 NC4 管道；console 完全不用它們。
- 反例：如果 console 選 `CONFIG_ESP_CONSOLE_USB_CDC`，那才會用到 TinyUSB ——
  那種 build 的 console 真的「要等 init」。我們的板用 UART／USJ，就是為了避開這件事。

S3 的 B10 之所以是 console 的分界：TinyUSB 初始化時拿走**同一條 FSLS PHY**，
USJ console 就此消失（不是我們主動關它）。

---

## 2. 時間線

```text
通電
 │  IDF console 已經在工作（UART0／USJ）
 │
 ├─ B01  nc_log_capture()：讀級別、配環
 │        log_debug_mode=1 → 即刻裝 hook（之後 ESP_LOG 只入環，console 靜）
 │        log_debug_mode=0 → 唔裝 hook（ESP_LOG 照出 console）
 ├─ B02  log_debug_mode 生效（2 KB ↔ 8 KB）
 │        …… B03–B09 每個階段照樣印 log ……
 │
 ├─ B10  ★ nc_log_start()：裝 hook（正常模式）、建 nc_log task、開始送環
 │        → 出口由 console 轉到「管道」（USB CDC 或 USJ）
 │
 └─ B11–B12 之後只剩管道
```

---

## 3. 兩種模式

| | `log_debug_mode=0`（正常） | `log_debug_mode=1`（除錯） |
|---|---|---|
| B01 起 | 不裝 hook；console 直接出 | 裝 hook；**console 靜**（除非開了 TEE） |
| 環 | 2 KB，B10 才開始入 | 8 KB，B01 開始入 |
| B10 之後 | 管道由 B10 的行開始 | 管道一次吐出 B01 起的（最多 8 KB） |

---

## 4. 依板子／設定

| 板／設定 | console | B01–B09 在哪看 | B10 之後 |
|---|---|---|---|
| S3（預設 USJ） | USJ | USJ（直接出） | 同一條 USJ，改由管道出 |
| S3（改 OTG，正常） | USJ | USJ | USJ 被 TinyUSB 拿走 → 新 CDC port **只見 B10 之後** |
| S3（改 OTG，除錯） | USJ | 靜（入環） | 新 CDC port **補回 B01 起** |
| C3 | UART0（＋USJ secondary） | UART0／USJ | UART0 停；log 走 USJ 管道 |
| P4-ETH（冇 USB，正常） | UART0 | UART0 | 一直在 UART0（冇 `nc_log_start`） |
| P4-ETH（冇 USB，除錯） | UART0 | **靜**（hook 收了但冇管道） | **仍然冇 output**（見 §6） |

> OTG 的細節：`usb_init()` 之後才 `nc_log_start()`。正常模式下，這兩行之間
> （TinyUSB 已拿走 USJ、hook 未裝）會漏掉幾行；除錯模式已經入環，不會漏。

---

## 5. 常見誤解

- 「B10 才開始有 log」→ 錯。B01 就開始印，只是走 console；B10 是**管道**開。
- 「B10 之後 console 還有」→ 除非 `CONFIG_NETCORE_USB_CONSOLE_TEE=1`
  （目前全局 off），否則 hook 會吃掉 console。
- 「換 OTG 之後看不到開機」→ 正常模式：舊 USJ 的行不會補到新 port；
  要完整記錄用 `log_debug_mode=1`。

---

## 6. 已知邊界：有 hook 但沒有出口

**沒有 USB、又開了 `log_debug_mode=1`** 的板（例如 P4-ETH）：
B01 裝了 hook，但全機只有這一條管道，而它永遠不會啟動 →
**整段開機都沒有 output**。

目前沒有自動退回 console 的機制（`nc_log_stop()` 可以還原，但沒有呼叫點）。
需要的話，可以在 B10 確認「沒有任何 log 出口」時卸回 hook；
代價是 B01–B09 已經入環的行不會補出（環只留在 RAM）。

---

## 7. 想看什麼就選什麼

| 想看 | 做法 |
|---|---|
| 即時、最早期 | 正常模式，看 console（UART0／USJ） |
| 一份由 B01 起的完整記錄（USB） | `log_debug_mode=1` ＋ 重啟，B10 後接 CDC／USJ |
| 開機後才接、看近期 | 正常模式；管道只留 B10 後的行（2 KB 環） |

---

## 8. 程式位置

| 事 | 位置 |
|---|---|
| 擷取（B01） | `nc_log_capture()`（[nc_log.c](../../firmware/components/sys/nclog/nc_log.c)） |
| 級別（B02） | `nc_log_set_levels()`（main.c `ph_storage`） |
| 管道（B10） | `nc_log_start()`；呼叫點 main.c `ph_usb`（USJ／CDC 各一） |
| console tee | `CONFIG_NETCORE_USB_CONSOLE_TEE`（只有 P4 的獨立 PHY 有意義；目前 off） |
