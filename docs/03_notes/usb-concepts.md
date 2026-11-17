# USB 概念整理：PHY、personality、function、NC4 line

> 目的：把現在混在一起的 USB 概念分層，避免 `usb_jtag`、`cdc`、`msc`、`usb.rx`、`usb_core` 互相當成同一件事。

## 1. 四層，不是一個 USB

| 層 | 是什麼 | 例子 | 由誰決定 |
|---|---|---|---|
| **PHY／port** | 實體 USB 控制器／D+/D- | S3 一個共用 PHY；P4 FS_PHY1、FS_PHY2、HS_PHY | 板子 hardware／board.json |
| **personality／driver** | 這條 PHY 用哪個 device stack | `usb_jtag`（USJ）／`cdc`（TinyUSB OTG） | config `links[].kind`＋編譯能力 |
| **function** | USB device 提供的功能 | NC4 CDC 資料、log、MSC 隨身碟 | `links[].msc`／`usb.cdc_log`＋link |
| **NC4 line** | 把 USB 資料當成 NC4 通道 | `usb_jtag0`／`cdc0` | `links[]`＋`routes[]` |

## 2. 目前實作對應

### PHY

- S3：OTG 與 USJ 共用同一條 PHY → 只能二選一。
- P4：三條獨立 PHY；目前 build 只承諾一條 USB link。
- `board.json features.usb` 目前**沒有被 code 讀**；真正開關是 `CONFIG_NETCORE_USB_OTG` 與 runtime config。

### Personality

- `kind: "usb_jtag"` → USB-Serial-JTAG（不是 JTAG debug，是 CDC-ACM）。
- `kind: "cdc"` → TinyUSB OTG。
- `usb_nc4_link()` 只取**第一條 enable 的 USB link**；多過一條 warning，未支援並存。

### Function

- `links[].msc`：只有 OTG 有意義；USJ 沒有 MSC。預設 0（唔交碟），global `usb.msc` 已停用。
- `usb.cdc_log`：只有 OTG 有意義；USJ 的 log 永遠開，和 NC4 共用 TX。
- MSC driver 目前無條件安裝（因為 TinyUSB descriptor 是編譯期決定）；`msc=0` 只代表不交碟。
- log 不是 NC4 line；它是 USB transport 上的一條文字管道。

### NC4 line

- `usb_jtag`／`cdc` link 是 NC4 的來源。
- `usb_core` 由 routes 決定；USB NC4 bus 是 PUSH。
- `usb.rx` 目前是**全域一份**；多條 USB line 需要 per-line buffer。

## 3. 混在一起的地方

| 問題 | 現象 |
|---|---|
| 「USB 線」和「NC4 link」同名 | `usb_jtag` 有時指 driver，有時指 config link |
| `usb.cdc_log`／`usb.rx` 是全域 | 多 USB port 時無法分開（`links[].msc` 已經 per-link） |
| `usb.rx` 是全域 | 多 USB link 時共用同一塊 |
| `usb_nc4_link()` 只取第一條 | P4 多 PHY 不能並存 |
| `board.json features.usb` 沒被讀 | 板子事實和 build 能力可能不一致 |
| P4 features.usb=false | 但 build 有 OTG；要靠 config 避免 |
| `cdc` 與 `usb_jtag` 的 log 規則不同 | USJ log 強制開；OTG 受 cdc_log |
| MSC 是 function，不是 line | 開關掛在 USB link（`links[].msc`），但唔會出現在 routes |
| `usb_core` 是 core affinity | 不是 USB 裝置編號 |

## 4. 目標模型

```text
usb_port0（PHY；板子事實，只註冊能力）
  ├─ personality 能力 = usj | otg
  ├─ speed = fs | hs
  └─ supports = nc4 / log / msc（msc 只限 otg）
        ▲
        │  claim（誰要用誰 claim，不是 config 配對）
   nc4／log／msc consumers
```

- **不要**在 config 寫 `functions` 或把 port 配給 service；這是 2026-11-17 撤回的方向。
- consumer 自己 claim：`nc4` 要一條資料通道、`log` 要出口、`msc` 要碟；registry 找
  同時滿足所有 claim 的 port，並推導 personality（有 MSC → OTG；否則有 USJ 用 USJ）。
- `usb.rx`／`usb.cdc_log` 將來是 **per-port 狀態**，由 claim 產生，不是 config 欄位（`links[].msc` 已經 per-link）。
- `usb_nc4_link()` 應該變成「registry 分配」，不是 first-only。
- `board.json features.usb` 應該 gate port 是否存在；編譯能力只 gate driver 是否存在。
- P4 多 PHY：build／板子註冊三個 port，claim 自動分配；不需要 `usb.ports[]` 配對表。

## 5. 現況建議

| 板 | 建議 |
|---|---|
| S3 | `usb_jtag0`、msc0、cdc_log1；單一 PHY，保持現在 |
| P4 | 保持 `uart0` + `features.usb=false`；若要 OTG，另立 `cdc0` + HS/FS PHY profile，不要和 USJ 混 |
| 多 USB port | 先做資源註冊＋claim（`nc_usb_port_register`／`nc_usb_claim`）＋per-port `usb.rx`，再談並存 |

## 6. 一句話

> 現在不是「USB 有很多功能」，而是 **PHY、driver、function、NC4 line 四層被同一個 `usb` 字包住**。
> 先把四層拆開，USB 配置才會清晰。目標是**註冊資源、consumer claim**，不是 `functions` 配對。
