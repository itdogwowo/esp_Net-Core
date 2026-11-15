# 網絡架構審視：繞過 decoder 的分支

> 原則：NC4 網絡協議應走
> `line → nc4_bus slot → parser → decoder → action`；
> HTTP web 是獨立 service，不應冒充 NC4。

## 1. 已修正

| 項目 | 之前 | 現在 |
|---|---|---|
| HTTP `/ota` | `nc4_ota_http.c` 直接 `esp_ota_begin/write/end`，不經 bus／decoder | **已刪除**；`/ota` 不再註冊 |
| Web UI OTA | 首頁有 `.bin` 上傳，POST `/ota` | 已移除；頁面只留靜態說明 |
| HTTP `/config.json` | `nc4_http.c` 直接讀寫 FAT config | **已刪除**；`/config.json` 404 |
| HTTP 直接寫入 | `/ota`、`/config.json` PUT 都直接改狀態 | 已無任何 HTTP 寫入 endpoint |
| HTTP OTA 建置 | `nc4_ota_http.c` 在 bus_net CMake | 已從 CMake 移除，檔案刪除 |

保留的唯一 OTA 協議路徑：

```text
0x2201/0x2202/0x2203 OTA_BEGIN/WRITE/END
→ line → nc4_bus → parser → decoder → ota_action
```

## 2. 仍存在的 bypass／分支

| # | 位置 | 問題 | 影響 | 建議 |
|---|---|---|---|---|
| 1 | ~~`nc4_http.c` `/config.json` PUT~~ | **已移除** | HTTP 不再直接寫 config | 已完成 |
| 2 | `main.c` `tcp_nc4_accept_task` | `nc_net_lines_enabled()==false` 時，main 自己開 TCP socket 直接餵 bus | 繞過 line/service 抽象 | 移除 legacy fallback，或限 compile-time |
| 3 | `nc4_line_net.c` | 舊 WiFi line：自己開 TCP/UDP/WS + httpd | 與 `nc_net_lines` 兩套 line | 只保留新 line；舊檔 compile out |
| 4 | `nc4_ws.c` | 舊 httpd WS 在 httpd task 內直接 parser，不經 bus | WS 路徑不一致 | 刪除或只留測試 |
| 5 | `nc4_udp.c` | 舊 UDP 直接 recv→parser | 同上 | 刪除或只留測試 |
| 6 | `nc_net_raw.c` | :5000/:5001 raw test endpoint，繞過 NC4 | production 可被 `rawbench` 開啟 | 移到 test build／`NC_ENABLE_TESTS`，不要留在 release |
| 7 | `nc4_bus` PULL mode | 舊模式可讓 decode task 自己讀 driver | 與 PUSH slot 模型並存 | 新線全部 PUSH；PULL 只留 legacy |
| 8 | HTTP `service=web` | HTTP 不經 decoder | 設計上係另一個 service，不一定是 bug | 文件明確：web 只做 UI／管理，不做 NC4 |

## 3. 核心綁定現況

### 新 NC4 line

```text
system.json nc4.routes[].out
→ nc4_router_core(line)
→ line.core
→ nc4_bus.cfg.core
→ xTaskCreatePinnedToCore(..., line.core)
```

- 每條 line **啟動時**綁 core；不是 per-request。
- `nc4_router_core()` 是查表函數；真相是 `routes[].out`。
- `nc4_bus_kick()` 只喚醒同核 decode task；跨核會計入 `st_kick_remote`（設計要求 0）。
- decoder 是 **per-core 軟體物件**，不是硬件；線程共用同核 decoder。

### HTTP line

```c
ln->core = 0;   // web 不需要 decoder；core 只係 httpd task
```

- 固定 core0。
- 不建立 `nc4_bus`、不找 decoder。
- 這是 service 區分，不是 NC4 line。

## 4. 建議的目標架構

```text
service=nc4:
  TCP/UDP/WS line → nc4_bus → parser → decoder（per core）

service=web:
  HTTP line → httpd → 靜態 UI（GET / 只有 HTML）
  沒有任何 POST/PUT、沒有 OTA、沒有 config 寫入
```

- OTA 只保留 NC4 0x22xx。
- `rawbench` 只在 test image。
- 舊 `nc4_line_net`／`nc4_ws`／`nc4_udp`／main direct TCP 全部 compile out。
- `/config.json` 已移除；HTTP 只剩靜態 `/`。

## 5. 驗證

```sh
python3 make.py check ESP32-P4-ETH
python3 make.py check ESP32-S3-N16R8
```

- 已通過：0 errors / 0 warnings。
- `/ota` 已不在 HTTP handler 表。
- NC4 OTA 仍由 `ota_action` 0x22xx 經 decoder 執行。

## 6. 線路身份與 iface 的現況（重要）

目前 `system.json` 的 `nc4.links[]` 有：

```json
{
  "name": "TCP",
  "kind": "tcp",
  "port": 6000,
  "iface": "eth",
  "service": "nc4"
}
```

但 `iface` 目前只在 log 出現：

```c
ESP_LOGW(TAG, "line %s：iface=%s ...", l->name, l->iface, ...);
```

真正 bind 仍然係：

```c
addr.sin_addr.s_addr = htonl(INADDR_ANY);
```

所以：

- 同一個 port 在 ETH／WiFi 各開一條，**目前不能可靠分開**。
- 第二條 bind 可能失敗，或者兩邊都收到，取決於 lwIP／SO_REUSEADDR。
- `name` 是 routes 的唯一 key；`TCP6000` 不能區分 iface，除非名字本身有 iface（`eth:T6000`／`wifi:T6000`）。

### 建議 schema

```json
{
  "name": "eth:tcp:6000",
  "kind": "tcp",
  "iface": "eth",
  "port": 6000,
  "service": "nc4"
}
```

- `name` 仍係 routes key，但由 `iface:kind:port` 生成，保證唯一。
- `iface` 必須由 line 層真正用嚟 bind（iface-specific IP／netif），唔可以只係 metadata。
- 如果想用 numeric index，內部用 enum（0=eth、1=wifi），config 仍建議寫 `"eth"`／`"wifi"`，避免板子之間 index 漂移。

### 目前上限

| 限制 | 值 |
|---|---:|
| `NC_CFG_MAX_LINKS` | 8 |
| `NET_MAX_LINES` | 8 |
| `NC4_MAX_BUSES` | 8 |

所以「無數個 port」目前最多 8 條 line；要更多必須先提高這些上限（同記憶體預算）。
