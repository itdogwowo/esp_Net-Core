# 解碼路由表（Router × 引擎 × 任務）

> **一句話**：`routes` 決定每一幀「**誰來解、要不要轉出去**」；
> `out` 放 `decode0` 就是本地執行，放 `UART` 就是轉發——跟舊專案 `out: ["self"]` 同一個語意。
>
> **引擎不用宣告**：`decodeN` 有人指向就開；`fxN` 有 `.fxe` 指定那一核就開。

---

## 0. 三個清單＋一個推導

| 清單 | 在哪 | 是什麼 | 誰決定 |
|---|---|---|---|
| `links[]` | `system.json` | 收幀通道：怎麼收、緩衝多大 | 用戶 |
| `routes[]` | `system.json` | 這條來源交給誰（引擎或通道） | 用戶 |
| `tasks[]` | `tasks.json` | 常駐任務（舊稱 service，兩者同一件事）**草案，未定案** | 任務管理器 |
| **`engines`** | **不用寫** | `decodeN` / `fxN`，由下面的規則推導出來 | 系統 |

**推導規則：**

| 引擎 | 何時存在 | 跑哪一核 |
|---|---|---|
| `decodeN` | 任一 `routes[].out` 提到它 | **N**（名字就是核心編號） |
| `fxN` | 有 `.fxe` 的 manifest 宣告 `core: N`（或 `-1` → 預設核） | N |

所以不會出現「宣告了但沒人用」（浪費一個 task），也不會出現「有人用但沒宣告」（跑不起來）。
兩邊只有一份真相：`decode` 的真相在 `routes`，`fx` 的真相在檔案本身。

```jsonc
// system.json
{
  "version": 1,
  "cID": "",
  "frame_interval_ms": 33,
  "num_pixels": 676,

  "nc4": {
    "links": [
      { "name": "CDC", "kind": "cdc", "cfg": { "rx": { "mode": "pull", "region": 8205, "chunk": 1024, "budget": 8 } } },
      { "name": "UART", "kind": "uart", "cfg": { "rx": { "mode": "pull", "region": 1024, "chunk": 512, "budget": 8 } } },
      { "name": "TCP", "kind": "tcp", "cfg": { "port": 6000, "rx": { "mode": "push_frame", "region": 16410, "chunk": 8192, "budget": 8, "slots": 8, "slot_size": 8205 } } }
    ],
    "routes": [
      { "in": "nctx",     "out": ["decode0"] },   // 本機 → 自己（永遠第一行）
      { "in": "outbound", "out": [] },            // 本機 → 外（永遠第二行）
      { "in": "CDC",      "out": ["decode0"] },
      { "in": "UART",     "out": ["decode1"] },
      { "in": "TCP",      "out": ["decode0", "UART"] }
    ]
  }
}
```

```jsonc
// tasks.json
{
  "version": 1,
  "tasks": [
    { "name": "tcp_accept", "core": 0, "enable": 1 },
    { "name": "tcp_rx",     "core": 1, "enable": 1 },
    { "name": "nc4_udp",    "core": 1, "enable": 1 },
    { "name": "wifi", "core": 0, "extern": "idf" },
    { "name": "bt",   "core": 0, "extern": "idf" }
  ],
  "override": []
}
```

#### 每塊板子只列「真的有」的通道

| 板子 | `links[]` | 為什麼 |
|---|---|---|
| **P4** | CDC、UART、TCP | ETH 真的起來，TCP bus 真的建 |
| **S3** | CDC | 沒有 ETH → 沒有 TCP / WS / UDP；UART 也沒接 |

**沒有的就不要寫**，需要的話照同一個三層形狀加（範例）：

```jsonc
{ "name": "TCP", "kind": "tcp", "cfg": { "port": 6000, "rx": { "mode": "push_frame", "region": 16410, "chunk": 8192, "budget": 8, "slots": 8, "slot_size": 8205 } } },
{ "name": "WS",  "kind": "ws",  "cfg": { "port": 8080, "rx": { "mode": "message",  "max_msg": 24576, "budget": 8 } } },
{ "name": "UDP", "kind": "udp", "cfg": { "port": 6001, "rx": { "mode": "datagram", "max_msg": 1472,  "budget": 8 } } }
```

routes 也要補：`{ "in": "WS", "out": ["decode1"] }`、`{ "in": "UDP", "out": ["decode1"] }`。
WS / UDP 是訊息導向，沒有 `region` / `chunk`；`max_msg` 就是一次收的最大長度。

`tasks.json` 同理：S3 的 `tasks` 是空的——沒有 network 就沒有 IDF 的 Wi-Fi / BT 任務，
要等 `network.json` 真的把它們開起來，才加 `{ "name": "wifi", "core": 0, "extern": "idf" }`。

> ⚠️ 現況有一個相反的例子：P4 的 firmware 在 ETH 起來時**自己**開 WS / UDP，config 沒列。
> 等 config 接管之後就會變成「沒寫就唔開」——到時要決定：把這兩行加回 P4 的 config，
> 還是接受預設關閉。在那之前，config 只是紀錄。

**每一條 link 都寫 `rx`，即使現在只有一個值。** 這些數字全部是「每條線不同、而且會被調」的東西：

| 欄位 | 現在的值從哪來 | 為什麼要放 config |
|---|---|---|
| `mode` | CDC / UART 只有 `pull` 的實作；TCP 兩種都有 | 現在是記錄，等 push 路徑補上就變真開關 |
| `region` | `nc_plan` 宣告（8,205 / 1,024 / 16,410） | config 讀完才 declare，大小本來就該由設定決定 |
| `chunk` | 量測出來的（1,024 / 512 / 8,192） | 一次讀太少會多繞幾圈，直接影響吞吐 |
| `budget` | 框架預設 8 | 公平性旋鈕 |
| `slots` / `slot_size` | TCP PUSH 用（8 × 8,205） | 記憶體 vs 掉幀的直接取捨 |

> `region` 不是純宣告：它會餵給 `nc_plan_declare()`。開機順序是 ④ 讀 `system.json` → ⑤ 宣告，
> 所以「config 決定多大」在架構上是成立的——現在只是還沒接。

- `extern: "idf"` = IDF 自己建的（Wi-Fi / BT），管理器只記帳、只印表，改不了它的優先權；
  `enable` 的真相在 `network.json`，這裡不重複。
- `router` **沒有 enable**：路由是必然的，寫了 routes 就生效。
- `override[]` = 對**任何**任務（包括自動推導出來的 `decodeN` / `fxN`）覆蓋 `prio` / `stack` / `core`。
  這是「調校」不是「設計」，開機一定要印 `decode0 prio 4 → 6 (override)`。

---

## 1. 舊專案的 `self` / `vbus`

| 名字 | 在哪一欄 | 意思 | 新專案對應 |
|---|---|---|---|
| `self` | `out` | 本地執行 | **`out: ["decode0"]`／`["decode1"]`** |
| `self` | `in` | 本機產生的幀 | `in: "nctx"`（改名，語意不變） |
| `vbus` | `in` | 本機虛擬總線的名字 | 移除（本機就是 `nctx`） |
| `vbus` | `out` | 無效，被忽略 | 移除 |

舊專案「本地解碼」就是 `out` 裡的一個目的地 `self`；現在有兩個解碼核心，
那個目的地就變成 `decode0` / `decode1`。**不需要 `exec` 欄位，也不需要宣告引擎。**

---

## 2. 四種組合

| `out` | 行為 | 舊寫法 |
|---|---|---|
| `["decode0"]` | 只本地執行 | `out: ["self"]` |
| `["UART"]` | 只轉發 | `out: ["uart0"]` |
| `["decode0", "UART"]` | 執行 ＋ 轉發 | `out: ["self", "uart0"]` |
| `[]` | 丟棄（明確不要） | `out: []` |

`out` 只接受兩種名字：`links[]` 的通道，或 `decodeN`（N = 核心編號）。
`fxN` **不能**放進 `out`——它不吃幀，按自己的 fps 跑。

### 2.1 本機發出的三條路（`exec` / `nctx` / `outbound`）

「本機」不是一條通道，它是三種不同意圖。同一個 `in` 只能有一行，所以意圖要分開命名：

| 情況 | 走法 | 幀 | 經過 router | 寫法 |
|---|---|---|---|---|
| 程式內部呼叫 handler | `exec`（直接 dispatch） | ❌ | ❌ | 不需要 route |
| **本機 → 自己** | 注入一幀，來源 `nctx` | ✅ | ✅ | `{ "in": "nctx", "out": ["decode0"] }` |
| **本機 → 外** | 注入一幀，來源 `outbound` | ✅ | ✅ | `{ "in": "outbound", "out": ["TCP"] }` |

- `exec` 最快，但沒有 CRC、沒有位址檢查、沒有路由——語意是「我呼叫一個函式」。
- `nctx` / `outbound` 都是真幀，所以位址、CRC、轉發規則一律照走。
- 為什麼要兩個名字：如果只寫 `{in:"nctx", out:["decode0","TCP"]}`，每一幀本機指令都會**又執行又外送**，
  想只做其中一件就表達不到。名字講的是**意圖**（給自己／給外面），不是總線——
  這也是舊專案 `vbus` 失敗的原因（`vbus` 是「一條總線」，跟 `self` 的語意重疊，最後變成表裡有、永遠查不到）。

**`nctx` 與 `outbound` 永遠存在，而且排最前**（舊專案 `self` / `vbus` 也是開頭兩個）。
沒寫就自動補：`nctx → ["decode0"]`、`outbound → []`（沒指定去哪就丟棄，開機表標 `default`）。

> **狀態**：`outbound` 是提案，未進 config。等真的要做「本機外送」那天再加一行。
>
> **為什麼叫 `outbound` 不叫 `emit`**：`emit` 是事件系統的字（JS EventEmitter、Qt `emit`、Vue `$emit`），
> 在路由／網絡語境不慣用；那邊的慣用字是 ingress / egress、inbound / outbound。
> `egress` 最精確，但對非網絡背景的人太專門，所以取 `outbound`。

---

## 3. 由舊到新

| 舊 | 新 |
|---|---|
| `{"in":"uart0","out":["self"]}` | `{"in":"UART","out":["decode1"]}` |
| `{"in":"now","out":["uart0"]}` | `{"in":"NOW","out":["UART"]}` |
| `{"in":"net","out":["self","uart0"]}` | `{"in":"TCP","out":["decode0","UART"]}` |
| `{"in":"self","out":["now"]}` | `{"in":"nctx","out":["NOW"]}` |
| `{"in":"...","out":[]}` | 同 |
| `{"in":"vbus",...}` | 刪掉 |

舊邏輯名 → 新名字：`now`→`NOW`、`net`→`WS`、`udp`→`UDP`、`uart0/1`→該條 link 的 `name`；
`nctx` 只當來源（舊名叫 `self`）；`vbus` 移除。

---

## 4. 規則與失敗處理

| 情況 | 行為 |
|---|---|
| `in` 不是 link、也不是 `nctx` / `outbound` | 開機警告 + 忽略該行 |
| 沒寫 `nctx` / `outbound` | 自動補在最前：`nctx → ["decode0"]`、`outbound → []` |
| 同一個 `in` 兩行 | 開機錯誤，兩行都不生效 |
| `out` 有未宣告的通道 | 警告 + 忽略該項，其餘照做 |
| `out` 有 `fxN` | 警告 + 忽略（fx 不吃幀） |
| `out` 有 `decodeN`，但板子沒有第 N 核 | 開機錯誤（S3/P4 都是 2 核，所以只接受 0/1） |
| `out` 含 `in` 自己 | 警告 + 忽略（自我反射） |
| 某條 link 沒有 route | 預設 `out = ["decode0"]`，開機表標 `default` |

**與舊專案的差異**：舊 Router 沒配對＝丟棄；新表沒配對＝給 `decode0`。
路由是必要資訊，靜默丟幀比默默指派更難查。要嚴格就開 `boot_strict: 1`。

---

## 5. 開機表（含推導結果）

```
kind      name         core  prio  src
engine    decode0      0     4     route nctx, CDC, TCP
engine    decode1      1     4     route UART
engine    fx1          1     3     3 × .fxe（manifest core=1）
engine    fx0          -     -     （沒有 .fxe 指定 core 0）
task      tcp_accept   0     9     tasks.json
task      tcp_rx       1     10    tasks.json
task      wifi         0     23    idf
route     nctx     → decode0           config
route     outbound → （無）             default
route     CDC      → decode0           config
route     UART     → decode1           config
route     TCP      → decode0, UART     config
```

- `engine` 那一組標明「為什麼存在」（`route ...` / `N × .fxe`），出問題時一眼看得出。
- 開機會把「有線路但沒有 route」的來源自動補一行 `{ "in": 線路, "out": [] }`，並寫回 config——
跟舊專案一樣。這是「用戶沒有機會知道」的補寫：新線路是程式建的，用戶不會知道要自己加 route。
用戶寫過的值一律不動。

---

## 6. 優先權與配額

**優先權不是用戶設計的欄位。** 它要跟 producer、Wi-Fi 保持一致，改錯只會靜默變慢：

| 優先權 | 誰 | 屬於 |
|---|---|---|
| 23 | Wi-Fi / BT | 外部（IDF） |
| 10 | `tcp_rx`、`nc4_udp` | task |
| 9 | `tcp_accept` | task |
| 4 | `decode0` / `decode1` | engine（推導） |
| 3 | `fx0` / `fx1` | engine（推導） |
| 2 | `slow_worker` | task |

要調的人寫 `tasks.json` 的 `override`，那是「覆蓋」不是「設計」。

**配額（`budget`）放在 `links[]`**：一條線最多吃幾幀才換人，預設 8。
舊專案的 `Buffer.decode_budget_slots`（=32）是**全域**配額、先到先吃；
新的 `budget` 是**每條線**，天然公平。新表沒有全局上限（8 條線都忙 = 一輪 64 幀），
如果之後發現抖動，再加 `max_frames_per_pass`。

---

## 7. 與現況程式的落差

| # | 現況 | 要改成 |
|---|---|---|
| 1 | `main.c:2329` 只 `nc4_decoder_create()` 一次，兩核共用 | 每個被 routes 指到的 `decodeN` 一個 decoder、一份回覆緩衝 |
| 2 | `bus.core` 寫死（CDC/TCP = 0、UART = 1） | 由 routes 推導：這條 bus 給 `decodeN`，就掛到第 N 核 |
| 3 | `tcp_accept`（9）、`tcp_rx`（10）、`nc4_udp`（10）是裸 `xTaskCreatePinnedToCore` | 收進 `tasks[]`，由任務管理器建立與管 |
| 4 | `fx` 是一個固定 task（core 1、prio 3） | 依 manifest 的 `core` 分組，每一核一個 fx 引擎；`-1` → 預設核 |
| 5 | WS / UDP 在 httpd / udp task 內直接解析 | 納入 `links[]`，各自宣告 rx 區域 |
| 6 | 只有部分通道有 `send` | `out` 只能指向有 send 的 link |

在這六件事做完之前，`routes[]` 只是格式；改它不會改變行為。

---

## 8. 命名：`fx` 還是 `effect`？兩個格式是什麼？

你記得的「兩個格式」是 **payload 的編譯狀態**，不是兩種檔案：

| payload | `target_id` | 何時用 |
|---|---|---|
| `.wasm`（WAMR 直譯） | 0（通用） | 第一版就出這個；同一份可跑 P4 與 S3 |
| `.aot`（warmc 預編譯） | 1 = rv32-ilp32f、2 = xtensa | 某個效果實測 > 8 ms/幀 才單獨編 |

兩者都裝在同一個 `.fxe` 容器裡，`flags` bit0 = 有 AOT、bit1 = 有 manifest；
載入器看 `target_id` 決定能不能載（AOT 綁架構，wasm 不綁）。

容器本身只有一種 `.fxe`：
- **檔頭 48 B** 回答「怎麼載入」（格式 / 長度 / crc / target）
- **manifest 40 B** 回答「怎麼活在系統裡」（kind / core / layer / prio / fps / px / mem / budget / 指令）

命名建議：程式碼留 `fx`（已滿地都是），對人的文字用 `effect`（manifest 的 kind 就是 `EFFECT`）。

## 9. 取捨

- **不宣告引擎**：少一份會和 `routes` / `.fxe` 不同步的清單；代價是「這台機器有幾個引擎」要推導，
  所以開機表一定要把推導結果印出來。
- **`fx` 由檔案決定核心**：模組自己說要跑哪一核（manifest `core`），
  比在 config 再寫一次可靠——但也意味著 `enable` 的開關是「有沒有 `.fxe` 指定那一核」。
  要全域關掉就加 `"fx": { "enable": 0 }`（本版未做）。
- **`tasks.json` 獨立**：任務清單和「這台機器長什麼樣」（system）無關，它會隨功能增減而變。
