# 配置分層與開機順序（System / Driver / Service）

> **狀態**：設計（程式目前只有「System 宣告 + 服務管理器」，config.json 還沒接）
> **參考**：mp_Net-Core 的 `config.json`（System / Buffer / Network / SPI / I2C / UART / TFT / WS2812 …）
> **相關**：[writing-a-service.md](writing-a-service.md)、[rx-architecture.md](rx-architecture.md)、[architecture-flow.md](architecture-flow.md)

---

## 1. 兩個世界，不要混

| 世界 | 是什麼 | 誰啟動 | 有生命週期嗎 |
|---|---|---|---|
| **System（核心）** | 硬體、driver、**NC4 通道**、log/status | 固定順序的開機流程 | ❌ 沒有；它就是系統 |
| **Service（應用）** | 效果、UI、慢工作、自測 | `nc_service` 管理器 | ✅ declare → start → stop |

**NC4 通道屬於 System**：它跟「這顆晶片有幾條網路、幾個 UART」是同一件事，
不該由應用服務管理器去宣告、啟動、停止。應用服務是「可有可無的功能」。

```
                    ┌──────────── System（固定順序）────────────┐
  config.json ──►   │ Hardware → Driver → Network / NC4 通道    │ ──► log / status
                    └───────────────────┬───────────────────────┘
                                        │ 提供 bus / hub / handle
                    ┌───────────────────▼───────────────────────┐
                    │ Service（可選）：effects / UI / jobs / …   │
                    └───────────────────┬───────────────────────┘
                                        │ nc_task_register
                    ┌───────────────────▼───────────────────────┐
                    │ Tasks：誰跑在哪一核、哪一層、多少 stack    │
                    └───────────────────────────────────────────┘
```

---

## 2. config.json 的層（照舊專案延伸）

```jsonc
{
  "System": {
    "hostname": "p4-netcore",
    "cID": 0xF5F9,
    "debug_level": 3,
    "frame_interval_ms": 33,
    "boot_strict": 1
  },

  // ── 硬體：腳位、匯流排、裝置（跟舊專案一樣按 bus 分）──
  "Hardware": {
    "PIN":    { "enable": 1, "list": [ ... ] },
    "SPI":    { "enable": 1, "list": [ { "id": 0, "mosi": 11, "sclk": 12, ... } ] },
    "I2C":    { "enable": 1, "list": [ { "id": 0, "sda": 8, "scl": 9, "freq": 400000 } ] },
    "UART":   { "enable": 1, "list": [ { "id": 1, "tx": 7, "rx": 8, "baud": 921600 } ] },
    "ETH":    { "enable": 1, "mdc": 31, "mdio": 52, "ref_clk": 50, "phy_addr": 1 }
  },

  // ── driver：不屬於硬體、屬於「怎麼用」的參數 ──
  "Driver": {
    "ETH":  { "phy_type": "IP101", "dma_buf": 1600, "rx_desc": 16, "tx_desc": 16 },
    "USB":  { "enable": 0, "msc": 1, "cdc": 1 },
    "TFT":  { "enable": 0, "driver": "st7789", "width": 240, "height": 320 }
  },

  // ── 網路 + NC4 通道（這一段就是「走什麼通道、哪一核、幾格、多大」）──
  "Network": {
    "lan": { "ip": "10.10.1.50", "mask": "255.255.0.0", "gw": "10.10.1.1" },
    "nc4": {
      "links": [
        { "name": "TCP", "kind": "tcp", "port": 6000,
          "rx": { "mode": "push_frame", "core": 1, "slots": 4, "slot_size": 8205 } },
        { "name": "WS",  "kind": "ws",  "port": 8080,
          "rx": { "mode": "message", "core": 1, "max_msg": 24576 } },
        { "name": "UDP", "kind": "udp", "port": 6001,
          "rx": { "mode": "datagram", "core": 1, "max_msg": 1472 } }
      ],
      "decode": [
        { "name": "decode0", "core": 0, "prio": 4, "budget": 8 },
        { "name": "decode1", "core": 1, "prio": 4, "budget": 8 }
      ]
    }
  },

  // ── 任務：誰跑在哪（可由 config 覆蓋；缺就用程式預設）──
  "Tasks": {
    "override": [
      { "name": "decode0", "core": 0, "prio": 4, "stack": 6144 },
      { "name": "tcp_nc4_rx", "core": 1, "prio": 10 }
    ]
  },

  // ── 應用層（Service）──
  "Effects": { "enable": 0, "list": [] },
  "UI":      { "enable": 0 }
}
```

### 為什麼這樣分

| 層 | 判斷標準 | 例子 |
|---|---|---|
| **Hardware** | 「這顆晶片/板子上有這個腳位或裝置嗎」 | SPI0 的 MOSI/SCLK、ETH 的 MDC/MDIO、UART1 的 TX/RX |
| **Driver** | 「同一個硬體，用什麼方式驅動」 | PHY 型號、DMA 緩衝大小、TFT 驅動 IC、USB 是否啟用 |
| **Network / NC4** | 「通道怎麼開、跑哪一核、多少緩衝」 | link 陣列、decode 引擎的 core/prio/budget |
| **Tasks** | 「任務的排程參數」 | core / prio / stack |
| **Service（Effects/UI）** | 「功能，可開可關」 | 燈效、UI 頁面 |

**舊專案的 `Buffer`（u8_rx_slots 等）在這裡被拆掉**：每一條 link 自己寫 `slots` / `slot_size`，
因為 TCP 的 8 KB 幀與 UART 的 1 KB 幀需求差很多（見 testing §2.13）。

---

## 3. 開機順序（System 固定，Service 可選）

```
 ① NVS init
 ② config.json 載入（/data/config.json → 找不到就用出廠預置）
 ③ 硬體初始化      Hardware.*   （GPIO/SPI/I2C/UART/ETH PHY）
 ④ Driver 初始化   Driver.*     （ETH DMA、USB、TFT…）
 ⑤ 宣告（記帳）    ← ★ 在 ③④ 之後：**看硬體實際存不存在才決定要宣告什麼**
      · System：ETH 起來才宣告 tcp.rx；UART 存在才宣告 uart.rx…
      · Service：每個服務的 declare()
 ⑥ 資源 carve      nc_plan_commit(目前可用量)   ← 也在硬體之後量，才不會高估
 ⑦ NC4 通道啟動    Network.nc4 （建 transport → 建 bus → 掛 decode 引擎）
 ⑧ 應用服務啟動    nc_service_start_all()（效果、UI、自測…）
 ⑨ 任務啟動        nc_tasks_boot() 依 layer（L0 transport → L1 decode → L2 app）
```

**為什麼 ⑤ 在 ③④ 之後（而不是之前）**：
宣告的內容**取決於偵測到的硬體**。config 寫「ETH enable=1」不代表 PHY 真的 link 起來；
link 不起來就不該宣告 tcp.rx，否則會白白占掉 16 KB，而且通道建了也不能用。
程式裡的做法：`net_eth_start()` 成功才把 `s_eth_ok` 設起來，`svc_net_declare()` 才把 `tcp.rx` 加進去。

**為什麼 ⑥ 也在 ③④ 之後**：硬體/driver 會先吃掉 DMA 與 heap（ETH 16×1600×2 ≈ 51 KB）。
先量再 carve，才不會「宣告時看起來夠、真的要的時候不夠」。

**為什麼 ⑦ 在 ⑥ 之後**：通道的 slots/parser 是 carve 出來的，沒有 ⑥ 就沒有記憶體。

---

## 4. NC4 通道初始化（System 的第 ⑦ 步）

```
for each link in Network.nc4.links:
    transport = tcp_ws_udp_create(kind, port)          // 只開 socket / 握手
    bus       = nc4_bus_create(link.name, rx.mode, rx.slots, rx.slot_size)
    bus->slots = plan_get("<link.name>.slots")         // ⑥ 已經 carve 好
    decode    = decode_engine_by(link.rx.core 或 decode 表)
    decode_attach(decode, bus)
    transport_start(transport, bus)                    // 起 RX task / httpd / udp task
```

`decode` 引擎本身也是 System 的一部分（它在 L1 起），不是 Service。

---

## 5. 程式對應（現況 vs 目標）

| 層 | 現在 | 目標 |
|---|---|---|
| config 載入 | ❌ 沒有（用程式常數 + NVS） | `components/sys/config/`：cJSON → `nc_cfg_t` |
| 硬體 | `main.c` 直接寫死（ETH 的 GPIO、UART 腳位） | `ports/<board>` 提供預設，config 覆蓋 |
| NC4 通道 | `main.c` 的 `tcp_nc4_*`（P4-only） | `sys/transport/` + `Network.nc4.links` |
| 資源 | `nc_plan`（5 項）＋ 插槽 `calloc` | 全部走 plan（含 slots） |
| Service | `nc_service`（4 項，含系統） | 只剩應用服務；System 走自己的順序 |

**這輪已經改的**：
- 開機順序改成「硬體 → 宣告 → carve → 通道」，宣告看實際硬體（`s_eth_ok`）。
- `nc_service` 管理器上板（declare → start，逐項失敗）。
- **System / Service 已經分開**：`main.c` 的 `s_sys_parts[]`（nc4 / net / core）走固定順序，
  `nc_service` 只剩應用服務（目前是 `probe` 示範）。`0x1101 keys=svc` 只看得到應用服務；
  `keys=mem` 的 plan 看得到全部（含 System）。

**還沒改**：config.json 載入、slots 走 plan、硬體/Driver 段接手 main.c 的寫死值。

---

## 6. 遷移步驟

| 步 | 做什麼 | 驗收 |
|---|---|---|
| C0 | 把 `net`（UART/TCP 緩衝）從 `nc_service` 移到 System 宣告/啟動 | `keys=svc` 只剩應用服務；bench 不變 |
| C1 | `sys/config` 讀 `data/config.json`（cJSON），先只支援 `Network.nc4.links` | 改 link 的 slots/port 重開機生效 |
| C2 | 硬體/Driver 段接手 `main.c` 的寫死值（ETH 先） | 拔掉 config 仍用內建預設 |
| C3 | slots 走 plan（`net.<link>.slots`） | `keys=mem` 看得到、`copied=0` |
| C4 | `Tasks.override`（core/prio/stack） | 改 config 的 core 重開機生效 |

> 原則：**程式裡永遠有預設值**；config 壞掉或欄位缺 → 用預設，不要讓系統起不來。
