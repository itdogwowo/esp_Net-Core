# 系統模型：System 組件與核心組

> 一句：**System 組件唔屬於任何核；它做的是建立每個核的「核心組」——
> 解碼器是根，任務疊在上面。核不存在，整個組就在仍然存在的核建立。**
>
> 這份是**設計理念**（由上而下）。核隔離的要求在 [core-isolation.md](core-isolation.md)，
> 任務本身的模型在 [task-model.md](task-model.md)，路由細節在 [routing.md](routing.md)。

---

## 1. 三層

```text
System 組件（main + components/sys/*）              ← 唔屬於任何核；B01–B12
  │
  ├─ 問：這塊板有幾多個核？                          B03 capability
  │
  ├─ 對每個核建立一個「核心組」                       B07 declare ／ B09 start
  │   core0 組
  │    ├─ 解碼器（根）          ADDR 過濾、dispatch、回覆、執行鎖
  │    ├─ 通道（link）          USB／UART／TCP／…：收到幀就交給這個解碼器
  │    └─ 任務（租戶）          decode0、fx、matrix、probe…
  │   core1 組  ── 同構，完全獨立
  │
  └─ 核唔存在 → 整個組落到仍然存在的核               B03 收斂
```

職責分界：

| 層 | 知道什麼 | 不知道什麼 |
|---|---|---|
| **System 組件** | 有幾多核、每組在哪、幾時建立 | 任務做什麼 |
| **核心組** | 自己嗰核的解碼器、身份、通道、任務清單 | 另一個組的存在 |
| **任務** | 自己要做什麼、要幾多緩衝 | 跑在哪一核（它只講「要求」） |

「System 組件」不是一個新的 component，而是現有的
`firmware/main` ＋ `components/sys/*` 合起來的角色：它提供開機、config、任務管理器、
解碼器、緩衝，令上面的任務只需要宣告「我要什麼」。

> **現時「組」是隱含的**：它沒有一個 struct，是由「解碼器陣列 ＋ 各任務的 affinity
> ＋ 同一個落點函式」合起來表達。把它顯式化成一個組物件是下一步（見 §6）。

---

## 2. 為什麼解碼器是根

一個核**唯一**與外界接觸的入口，就是它的解碼器：

- 每條線收到的每一個幀，都經該核的 decoder 過 ADDR／CRC → dispatch → 回覆；
- 任務不自己收幀；它上面的世界（指令、檔案、效果）都是由解碼器派下去的；
- 回覆緩衝、執行鎖、`cid`／`master_cid` 是**組**的狀態，不是單一任務的。

所以「一個核」＝「一個解碼器 ＋ 它上面那層任務」。
`nc4` 任務自己的兩個 loop（`decode0`／`decode1`）就是兩個組各自的「根任務」。

補充兩點，免得把「根」講成「每組一定要有一份」：

- **有線的組一定有解碼器**：沒有解碼器就沒有人收幀，那條線等於死。
- **沒有線的組可以不建解碼器**：沒有人要收幀，建了只是白佔 2,615 B 回覆幀。
  這是**資源**結論（「組有此需要嗎」），不是落點收斂。

---

## 3. 完全分離

兩個組 = 兩個獨立系統，唯一接口是緩衝（[core-isolation.md](core-isolation.md) 的 I1–I3）。
由這個前提直接推出：

1. 一個組的任務不可以借另一個組的解碼器收指令 —— 每組有自己那份。
2. 跨組轉發要經緩衝／轉寄 FIFO，不可以「順手用對面嗰個 decoder」。
3. 「兩個核各做一組」與「一個核做兩組的事」是兩種形狀，不可以混：
   前者兩份解碼器、兩把鎖；後者只有一份，所有解碼工作序列化（這是單核的物理事實）。

---

## 4. 落點規則（唯一一條）

> **一個核的核心組，在仍然存在的核建立：優先自己那核，否則 core0。**

```text
requested core ──(核存在?)──> home = requested
                    └─(唔存在)──> home = 0   （現時仍然存在的核）
```

- config／程式寫的 `routes[].out[]` 裡的 `decodeN`、`tasks[].affinity`、`loops[].core`、
  `bus.cfg.core`、net task 的釘核 —— **全部只是「要求」**。
- 「要求 → 落點」由**同一個函式**答：`nc_core_clamp()`（語義 = home）。
  其他任何地方再判一次，就會出現「一半用 requested、一半用 home」的死線：
  bus 記住 core1，但 core1 冇 decode 任務 → `owner_init` 永遠不跑，decode0 也不巡那條。
- **第二條問題要分開問**：「這個核有冇解碼器？」（`s_ndec`／`decoder_slot()`）
  答的是「有冇線路由到它」，不是「核在不在」。兩者都答 0 時看起來一樣，
  但語義不同 —— 混在一起就會把「冇線」誤當成「冇核」。
- **USB 線要分兩個問題**：B03 決定人格（邊條線做 USB、CDC／USJ、log／msc）；
  B05 建「線 → 組」表時決定它入哪一組，並寫入 `ctx.usb_core`。
  之後 B07（回覆幀）、B10（TinyUSB／log）、B11（bus）只讀 ctx —— 不再各自問 router。
- 推論：
  - **單核**：所有核的要求 home = core0 → 整個 core1 組（解碼器＋任務＋通道的核歸屬）
    在 core0 建立。這是模型的推論，不是「解碼器特例」。
  - **雙核但 core1 冇線**：不建 decoder1（省 2,615 B 回覆幀），
    但如果 `tasks.json` 有任務指定 core1，它們仍然在 core1 跑 ——
    只是「沒有從 core1 收幀」的通道，收指令一律經 decoder0。
  - **身份（`cid`／`master_cid`）屬於組**：在解碼器建立之後才套（B07）。
    B02 讀到的值只是「這次開機的決定」，不是「已經落到 decoder 身上」。

---

## 5. 建立次序（與 B 階段對照）

| 階段 | System 組件 | 對應 |
|---|---|---|
| **B03** capability | 問核數；把「要求」收斂成「落點」（config 的 `decode1`／core1 → core0）；再定 USB 人格 | `nc_core_probe`／`nc_cfg_merge_cores`／`usb_resolve` |
| **B05** hardware | 線 → 組（router）；USB 那條線的組寫入 ctx 一次（之後只讀） | `nc4_router_set`／`usb_core` |
| **B07** declare | 對每個有線路由到的核：建解碼器（根）＋套身份；向 plan 要回覆幀 | `s_ndec`／`nc4_decoder_create`／`decoders_set_identity` |
| **B09** start | 組內任務：由任務管理器建立 loop；loop 的核 = 落點 | `nc_task`／`nc_core_clamp` |
| **B11** channels | 通道：bus 的核 = 落點；producer 同核 | `ph_channels`／`decoder_core` |

次序的理由：落點（B03）一定要早過「線 → 核」（B05）與「建組」（B07）；
B07 一定要早過 B09（回覆幀的數目決定資源次序），B11 最遲（要等任務與解碼器都在）。

---

## 6. 現況對照

**已落地**：

- 核數與落點集中在 `nc_core`（`nc_core_clamp()`）；單核時 config 的兩核要求在 B03 收斂。
- 解碼器那一組設定（decoder 索引、`bus.cfg.core`、producer 釘核、回覆幀索引）由
  `decoder_slot()`／`decoder_core()` 一起回答；核不存在時它與 `nc_core_clamp()`
  給同一個答案（core0）。
- 身份在 B07 建立之後套（之前寫在 B02，等於沒有套）。
- 沒有線的組不建解碼器（`s_ndec` 只看「有冇線路由到 core1」）。

**尚未**：

- 任務管理器仍然以「任務」為單位，不是以「組」為單位；組是隱含的
  （由各任務的 affinity ＋ `nc_core_clamp()` 表達）。
- P4 的 UDP／WS 生產者仍然釘 core1（有第二個解碼器時合理；單核 build 已 clamp 落 core0）。
- 沒有顯式的「組」物件／STATUS 欄位；目前用 `ndec`、router 表間接看。

---

## 7. 一句話

> System 組件不負責「任務做什麼」，它負責「有幾多組、每組在哪、幾時建」；
> 任務只負責「我要什麼」。核不存在，組就走位 —— 不是任務逐個自己 fallback。
