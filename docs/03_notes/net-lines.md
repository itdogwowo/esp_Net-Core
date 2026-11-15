# 網絡線路層（Line → Service）

> 更新：2026-11-16。舊版「iface + transport + port + service」已收斂成
> **kind + index**；HTTP 只做 static，不再是 line kind。

## 1. 三層

```text
Line（transport）
  kind = eth_tcp / wifi_tcp / eth_udp / wifi_udp / uart / cdc / usb_jtag
  index = 同類線編號
  name = kind + index（例：wifi_tcp1）
  port = network line 的 endpoint 參數

Service（consumer）
  HTTP static service：claim TCP :80，只提供 /data/www + fallback
  NC4 service：claim 其他 TCP/UDP line，建立 bus／parser／decoder
  WebSocket service：claim TCP :8080，做 handshake/framing，再交 NC4

Core
  每條 line 啟動時由 routes[].out 決定 decodeN；
  同一 interface 預設同一個 decode core。
```

## 2. Config

```json
{"kind":"wifi_tcp","index":1,"port":6000,
 "cfg":{"rx":{"region":16410,"chunk":8205,"budget":8,"slots":2,"slot_size":8205}}}
```

- 冇 `name` 會生成 `wifi_tcp1`。
- 冇 `iface`、冇 `service`。
- HTTP 不再寫 `kind:"http"`；用 `{"kind":"wifi_tcp","index":0,"port":80}`。
- `routes` 用生成後的名字：

```json
{"in":"wifi_tcp1","out":["decode0"]}
```

## 3. HTTP

- HTTP service 領取 TCP :80。
- 只 serve static `/data/www/`，有 fallback 頁。
- 冇 `/config.json`、冇 `/ota`、冇直接寫入。
- 所有操作（OTA、config、status）走 NC4；未來由 task manager 建 `task_http`。

## 4. 目前實作／限制

- 已支援 `eth_tcp`／`wifi_tcp`／`eth_udp`／`wifi_udp` + index + auto name。
- service claim 現階段用 well-known port：80=HTTP、8080=WS、其他=NC4。
- `iface` 仍未真正綁 local IP；同一 port 在 ETH/WiFi 並存未完成。
- UART／CDC／USB 仍用舊路徑，未全面 index 化。
- 上限：`NC_CFG_MAX_LINKS=8`、`NET_MAX_LINES=8`、`NC4_MAX_BUSES=8`。

相關：

- `line-identity.md` — kind/index 完整規範
- `network-architecture-audit.md` — 仍存在的 bypass／legacy
- `net-test-plan.md` — 測試方法
