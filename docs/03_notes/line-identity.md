# 線路身份：kind + index
> **2026-11-16 晚間定案：回到 composite kind（本頁的方案）。**
> `kind` 已含 interface（`eth_tcp`／`wifi_tcp`…），config 不再寫 `iface`／`service`；
> `name` 由 `kind + index` 生成，亦可以用 `name` 覆寫。落地細節見 [net-lines.md](net-lines.md)。


> 目標：網絡線、UART、USB 全部用同一套身份模型，不再分「實體線」和「網絡線」兩套世界。

## 1. 唯一身份

```text
line = kind + index
name = kind + index   （例如 eth_tcp0、uart1、cdc0）
```

- `kind`：實體／協議種類。
- `index`：同一個 kind 內的第幾條線（0 起）。
- `port`：網絡線的 endpoint 參數，不是身份。
- 不需要 `iface`。
- 不需要 `service`。

## 2. kind 一覽

### 網絡

```text
eth_tcp    eth_udp    eth_ws    eth_http
wifi_tcp   wifi_udp   wifi_ws   wifi_http
```

`eth_*` 一定綁 ETH netif；`wifi_*` 一定綁 WiFi netif。
`*_http` 是靜態 HTTP service；其他 `*_tcp/udp/ws` 直接是 NC4 line。

### 實體

```text
uart       cdc       usb_jtag
```

- `uart0`, `uart1`, … 對應 UART 控制器 index。
- `cdc0`, `cdc1`, … 對應 USB CDC interface index。
- `usb_jtag0` 對應 USB-Serial-JTAG。

## 3. 例子

```json
{"kind":"eth_tcp",  "index":0, "port":6000}
{"kind":"eth_tcp",  "index":1, "port":6001}
{"kind":"wifi_tcp", "index":0, "port":6000}
{"kind":"wifi_tcp", "index":1, "port":6001}
{"kind":"uart",     "index":0}
{"kind":"uart",     "index":1}
{"kind":"cdc",      "index":0}
```

生成：

```text
eth_tcp0
eth_tcp1
wifi_tcp0
wifi_tcp1
uart0
uart1
cdc0
```

## 4. 唯一性

`(kind, index)` 必須唯一。

所以以下是合法的，因為 kind 不同：

```text
eth_tcp0   port 6000
wifi_tcp0  port 6000
```

`port` 不是身份；同一 port 可以在 ETH／WiFi 各有一條。

## 5. routes

routes 的 `in` 用生成後的名字：

```json
{"in":"eth_tcp0",  "out":["decode0"]}
{"in":"eth_tcp1",  "out":["decode0"]}
{"in":"wifi_tcp0", "out":["decode1"]}
{"in":"uart0",     "out":["decode1"]}
```

## 6. 核心

- 每條 line 在啟動時由 routes 決定 `decodeN`。
- 預設：一個 interface 的所有 port → 同一個 decode core。
- `eth_*` 與 `wifi_*` 可以不同 core，但唔係必須。
- `index` 不代表 core；index 只是 line 編號。

## 7. 落地狀態（2026-11-16 晚間）

| 欄位 | 現在 | 目標 |
|---|---|---|
| `name` | 冇寫或空字串 → 由 kind+index 生成；非空 `name` 優先 | 已落地 |
| `kind` | composite（eth_tcp／wifi_tcp…） | 已落地 |
| `index` | 每條線都有 | 已落地 |
| `iface` | config 已移除；parser 由 kind 推導（bind 仍未分離） | 大致落地 |
| `service` | config 已移除；claim 仍由 port 政策決定 | 未完成（registry） |
| `port` | 網絡 endpoint，不是身份 | 不變 |

## 8. 上限

目前：

```c
NC_CFG_MAX_LINKS = 8
NET_MAX_LINES    = 8
NC4_MAX_BUSES    = 8
```

UART／USB／多 TCP port 要更多線，必須提高這三個上限，並重新計算 buffer／任務記憶體。

## 9. 重要限制

一條 TCP line = **一個 listener / port**，不是一個 connection。

同一 port 的多個 client 仍然由同一條 line 的 accept loop 處理；如果要每個 connection 都變成一條獨立 line，需要動態 line 建立，這是另一個設計。

## 10. 實作狀態（2026-11-16）

已落地：

- `nc_link_cfg_t.index` 欄位。
- parser 接受複合 kind：
  ```json
  {"kind":"eth_tcp","index":0,"port":6000}
  {"kind":"wifi_tcp","index":0,"port":6000}
  {"kind":"eth_udp","index":0,"port":6001}
  ```
- 無 `name` 時自動生成 `eth_tcp0`／`wifi_tcp0`。
- 複合 kind 自動帶 `iface`（eth／wifi），並設 `managed=true`。
- service claim 最小政策：
  - TCP `:80` → HTTP static service
  - TCP `:8080` → WebSocket + NC4
  - 其他 TCP/UDP → NC4
  - 舊 `kind=http/ws` 仍兼容
- HTTP line 不再需要 `kind=http`；`eth_tcp0:80` 由 HTTP service claim。
- 2026-11-16 晚間：七塊 project 的 `system.json` 已轉 composite kind + index；
  `uart`／`usb`／`wifi_*` 都加 `index`（USB 另外有 `kind:"usb"` 的硬件類別），
  並且**保留**生成名做預設（`usb0`／`uart0`／`wifi_tcp0`），唔再靠「冇 name」隱式生成；
  用戶寫 `""` 或省略時才 fallback 生成。firmware 的 UART lookup 已改成 by-kind（USB 本來就係）。
- S3 已實測：
  - `wifi_tcp0:80` → HTTP 200
  - `wifi_tcp1:6000` → NC4 TCP
  - `wifi_tcp2:8080` → NC4 WS
  - `wifi_udp0:6001` → NC4 UDP

仍未完成（下一步）：

1. 真正 service registry／claim state API（目前是線性掃描 + port 政策）。
2. iface-specific bind：`eth_*`／`wifi_*` 目前仍未綁 local IP，只是 kind 帶 iface。
3. UART／CDC／USB：config 已冇 `name`；firmware 已改成 by-kind，冇 by-name 依賴。
4. P4 device FAT 仍是舊格式（source config 已改）。
5. 提高 8 條 line／bus 上限。
