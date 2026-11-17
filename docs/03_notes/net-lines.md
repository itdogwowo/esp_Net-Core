# 網絡線路層（Line → Service）

> 更新：2026-11-16 晚間定案。line = **kind + index**；`kind` 是 composite
> （`eth_tcp`／`wifi_tcp`／`eth_udp`／`wifi_udp`／…），**不再寫 `iface`／`service`**。
> `name` 由 `kind + index` 生成，亦可以用 `name` 覆寫。`port` 是 endpoint，不是身份。

## 1. 三層

```text
Line（transport）
  kind  = eth_tcp / wifi_tcp / eth_udp / wifi_udp / eth_ws / wifi_ws / uart / cdc / usb_jtag
  index = 同 kind 的編號
  name  = <kind><index>（eth_tcp0／wifi_udp0／uart0）；可以 name 覆寫
  port  = 網絡 endpoint 參數；rx 直接放 link 頂層

Service（consumer；按 well-known port claim）
  HTTP static：tcp:80        → httpd；只 GET/HEAD /data/www + fallback；冇寫入
  WS         ：tcp:8080 或 kind=*_ws → 自寫 handshake／binary frame → nc4_bus
  NC4        ：其他 tcp/udp  → nc4_bus → parser → decoder

Core
  routes[].out 的 decodeN → nc4_router_core(line) → bus core（line 啟動時綁定）
  HTTP 固定 core 0（唔需要 decoder）；將來 HTTP task 化，core 由該 task 決定
```

## 2. Config（canonical）

```json
{"kind":"wifi_tcp","index":1,"port":6000}
```

- 冇 `name`，或者 `name` 係空字串 `""` → 由 `kind + index` 生成；
  routes 用**解析後**的名字。
- `iface` 不再是 config 欄位；interface 由 composite kind 決定。
- `service` 不是 line 欄位；claim 由 kind／port 決定。
- 舊寫法仍然讀：`kind:"tcp", iface:"wifi"`、`kind:"eth_tcp"`、`cfg:{port,rx}`；
  `python3 tools/board.py check` 會提醒。
- `name` 可以覆寫（非空才生效）；出廠 config 現在會寫上生成名做預設
  （`usb0`／`uart0`／`wifi_tcp0`／`eth_tcp0`），用戶可以改，或寫 `""` fallback 生成名。
  舊的 `usb_jtag0`／`cdc0` 只在「冇 project seed」時由晶片能力表生成。

**Name 與 Route 是兩個獨立的軸**：

- **Name 任何時候都存在**：唔寫就由 `kind + index` 生成（`eth_tcp0`）；寫了就用用戶的名。
- **Route 是 optional 的 policy**：它用 name 做 key，決定這條線的幀去 `decodeN` 還是轉去另一條 link。
- HTTP line 有 name（`eth_tcp0`）但**冇 route**，因為它不經 `decodeN`；不是「冇 name 所以冇 route」。
  同樣，一條 line 冇 route 也不會令它冇 name。

## 3. HTTP 邊界（2026-11-16 收緊）

- HTTP 只做 **static UI**：`GET/HEAD /data/www/*` ＋ fallback 頁。
- 冇 `/config.json`、冇 `/ota`、冇 PUT／POST 寫入 endpoint。
- OTA 唯一路徑：**NC4 0x22xx**（line → nc4_bus → parser → decoder → `ota_action`）。
- WebSocket 唔經 httpd：line task 自己 handshake／frame，之後交 `nc4_bus`。

## 4. 實作／限制

- managed line：composite kind（或舊 `iface`／`service`）→ `nc_net_lines` 接管。
- **`iface` 由 kind 決定**：socket bind 仍然係 `INADDR_ANY`；同一個 port 喺
  ETH／WiFi 同時存在時未能可靠分離（見 network-architecture-audit §6）。
- **`name` 覆寫**：`name` 是 optional；出廠 config 全部用生成名。firmware 的 UART／USB
  查找已改成 by-kind，所以用戶改名之後仍然找得到。
- 上限：`NC_CFG_MAX_LINKS=8`、`NET_MAX_LINES=8`、`NC4_MAX_BUSES=8`。
- legacy 路徑仍在：`main.c` tcp accept fallback、`nc4_line_net`、`nc4_ws`、`nc4_udp`
  （目標 compile out；見 network-architecture-audit §2）。
- `rawbench` :5000／:5001 係測試 endpoint（`network.json` 的 `rawbench=1` 才開），release 要關。

## 5. `rx` 要不要寫？

`nc_cfg_rx_default()` 現在就是 **2026-11-15 量測後**的公約數（net-test-matrix §4.6）：

| kind | 預設 |
|---|---|
| `*_tcp` | 2 × 8,205 B（region 16,410） |
| `*_ws` | 2 × 8,192 B（region 16,384） |
| `*_udp` | 1 × 8,205 B |
| `uart`／`cdc`／`usb_jtag` | 量測值（1 KB／4,128 B） |
| `*_http` | 不建 bus；`rx` 不會 allocate |

所以 P4-ETH／S3-N16R8 這種有 PSRAM 的板，config **不需要寫 `rx`**。
4 MB／無 PSRAM 的 Matrix 仍然要覆寫：1 slot、UDP 4,096（記憶體＋datagram 上限）。
`slot_size` 是 `recvfrom()` 上限，不是純記憶體；改細會截斷幀。

要減少 config 的正確做法是調整 `nc_cfg_rx_default()`，不是逐條刪 `rx`；
反過來，也不要把預設改回 8 slots（8 × 8,205 = 65,640 B／線，係未量測前的舊值）。

相關：

- `line-identity.md` — kind＋index 的身份模型（本頁是落地細節）
- `network-architecture-audit.md` — 仍存在的 bypass／legacy 清單
- `net-test-plan.md` — 測試方法
