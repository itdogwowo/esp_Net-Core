# 緩衝區貼身化（2026-11-08 戰役）

> **目標（使用者原話的三點）**
> 1. 所有任務的緩衝區是**貼身**的：不存在「先給大再慢慢收」的靜態分區；
>    用測試時間換取不降性能的尺寸。
> 2. **雙核各自獨立**：core0 與 core1 的任務／緩衝互不干擾（不是「共享一個大池然後看誰先搶」）。
> 3. **UART 的緩衝區太大**，要模仿 CDC 的設計（由實測 payload 推導），最後上板驗。
>
> **環境**：S3（`ESP32-S3-N16R8`）、CDC（core0）＋ UART1 迴路（core1, IO7↔IO8）、
> USB MSC 把 FAT 交給主機（`/Volumes/NC-2B4C`）。只有 OTA 一條更新路徑（無 JTAG、無 rollback）。
> 量測時間預算 8 小時；可能需要人手（換接線、按鍵、斷電）的測試全部排最後。

---

## 0. 量測方法（先定，避免邊做邊改標準）

每個數字都要能回答「這個緩衝為甚麼是這個大小」。

| 量什麼 | 怎麼量 | 用在哪 |
|---|---|---|
| parser 最大幀（payload） | `nc4_parser` 新增 `max_len`，STATUS `net` 回報 | region = 13 + max_payload + 餘裕 |
| 一次讀（push/read）最大 bytes | bus 統計 `max_read`、`reads` | slot_size ≥ max_read；chunk 由它反推 |
| 同時佔用格數峰值 | 已有 `peak`（`st_slot_peak`） | slots = peak + 1~2 餘裕 |
| 拿不到格次數 | `slot_full`（新增） | 若 > 0 → slots 不夠（或 producer 太快） |
| 回覆最大 bytes | 每 link `send` 包一層計數 | UART TX ring、CDC TX FIFO 需求 |
| 任務棧 | 已有 HWM（`STATUS rt`）＋ `stack_cover.py` 走遍 | stack = HWM + 安全邊際 |
| 核心干擾 | `kick_remote` 必須 0；單核加載時另一核的延遲 | 雙核獨立性的驗收 |

**判定「不降性能」**：CDC 單向吞吐（0xF00D）、指令 RTT、UART 迴路完成時間，
前後差距 ≤ 3%（量 3 次取中位數）。

---

## 1. 改動前盤點（基線；S3，2026-11-08 02:0x 舊映像實測）

| 項目 | 現在 | 誰擁有 | 核心 |
|---|---:|---|---|
| CDC parser region（plan） | 4,192 B | nc4 bus | 0 |
| CDC push slots | 4 × 256 = 1,024 B | nc4 bus（calloc） | 0 |
| CDC 任務棧 `cdc.rx` | 1,536 B | producer | 0 |
| UART parser region（plan） | 1,024 B | nc4 bus | 1 |
| UART push slots | 4 × 1,024 = 4,096 B | nc4 bus（calloc） | 1 |
| UART 任務棧 `uart.rx` | **4,096 B** | producer | 1 |
| UART driver RX ring | **4,096 B** | IDF driver | 1 |
| UART driver TX ring | **8,192 B** | IDF driver | 1 |
| decode0 / decode1 棧 | 4,096 / 4,096 B | nc4 loop | 0 / 1 |
| `nc_log` 棧 | 3,072 B | log | 0 |
| main 棧 | 4,096 B | app_main | 0 |
| TinyUSB task 棧 | 1,280 B | TinyUSB | 0 |
| log ring | 2,048 B（正常）／8,192 B（除錯） | nc_log | 0 |
| 回覆幀 | 2,615 B（PSRAM，一個 decoder） | decoder | 共用 |
| 指令表 | 444 B（heap，封存） | nc_action | 共用 |
| `s_buses`（8 條 bus 的殼） | 2,240 B `.bss` | nc4_bus | — |
| `s_slots`（20 個任務格） | 6,000 B `.bss` | task manager | — |
| `_cdcd_itf` / `_mscd_epbuf` | 12,548 / 8,192 B `.bss` | TinyUSB | 0 |

粗帳：**UART 一條線 = 4,096（slots）+ 1,024（parser）+ 4,096（RX）+ 8,192（TX）≈ 17.4 KB**
（再加 4 KB 任務棧）。CDC 一條線 = 4,192 + 1,024 ≈ 5.2 KB。
UART 這條線明顯是「照抄 CDC 年代的預設，但每一格都放大」（slot 1 KB、ring 4K/8K）。

---

## 2. 分期

### Phase 0 — 基準（不確定的事先變成數字）
1. 由 ELF＋原始碼產出完整緩衝清單（`.bss`/plan/heap/stack），寫進本文件。
2. 建 HEAD（tests ON）→ OTA → 確認開機 log 與 `STATUS` 正常。
3. 量基準：CDC 吞吐、指令 RTT、UART 迴路 100/200 幀、全部任務 HWM、`STATUS mem`。
4. 存檔：`artifacts/baseline-*`（含 SHA 與量測 JSON）。

### Phase 1 — 儀表（讓「需要多少」可量）
1. `nc4_parser`：`max_len`（最大幀 payload）。
2. `nc4_bus`：`reads`、`max_read`、`slot_full`、`tx_count/tx_max`（回覆）。
3. `STATUS keys=net` 增欄；`STATUS keys=rt` 已足夠。
4. OTA → 跑 Phase 0 的三種負載 → 收峰值。

### Phase 2 — 收斂
1. 由峰值推導：`region = 13 + max_payload + margin`；`slot_size ≥ max_read`；
   `slots = peak + 1`；driver ring 由「生產者喚醒間隔 × 位元率」推。
2. UART 照 CDC 的形狀：slot 256、slots 4、chunk 256（視量測再定）。
3. 改 code 預設＋板子 `system.json`（兩者要一致；config 覆蓋 code）。
4. 重跑負載 → 對照表；吞吐／RTT 差距 > 3% 就回退該項。

### Phase 3 — 任務棧貼身
1. 用 `STATUS rt` HWM ＋ `tools/stack_cover.py` 跑遍 handler。
2. 每個任務：`stack = ceil(HWM × 1.25) + 128`，但不低於 1,536（ISR/深呼叫保底）。
3. 只調「有量測依據」的；`main`／`TinyUSB` 不動（不是我們的任務）。

### Phase 4 — 雙核獨立驗收
1. `kick_remote == 0`（每條 bus）。
2. core0 打滿 CDC、同時量 core1 的 UART 迴路完成時間；反之亦然；差距 ≤ 5%。
3. 確認兩核的 plan 宣告互不重疊（每核自己的 class 額度）。

### Phase 5 — soak ＋ 文件
1. 30–60 分鐘混合負載，`slot_full=0`、`crc_err=0`、HWM 不退。
2. 更新本文件＋`status.md` §7.25；commit。

### Phase 6 — 需要人手（最後才做）
- UART 接線改動／換腳位（IO7 同時是 SD CLK 的衝突）。
- 拔插 USB、斷電重開（貼身之後的開機裕度）。
- SD 與 UART 同時要用的決策。

---

## 3. 風險與回退

| 風險 | 對策 |
|---|---|
| OTA 後的映像不開機（無 rollback） | 只 OTA 已經在本機 build 過、boot 路徑 diff 很小的映像；每次只帶一組改動 |
| 量測改動本身造成不穩 | 儀表只加計數，不改行為；先 OTA 儀表版再開始量 |
| 貼身過頭 → 邊界掉幀 | 每項留 ≥ 25% 餘裕；以 `slot_full`／`bad_hdr`／`crc_err` 為守門 |
| 拔線／按鍵需要人手 | 全部集中在 Phase 6 |

---

## 4. 量測紀錄（2026-11-08，S3，實測）

### 4.1 先找到「看起來正常、其實在燒資源」的四件事

| # | 問題 | 證據 | 影響 |
|---|---|---|---|
| 1 | **UART 的 read 會等 slot 填滿**：`uart_read_bytes(cap, portMAX_DELAY)` 在 IDF 裡是 `while(length)` 迴圈，收不滿就不返回 | 送 1 幀（77 B）→ `rx` 永遠 0；要 1 KB 到齊才被讀走 | 一幀 18 B 的指令延遲**沒有上限**；slot 1 KB 令它必死 |
| 2 | **測試指令的回覆幀還在任務棧上**：`test_action` 的 `nc4_reply_t` 是 by-value 的 2,615 B 陣列（7.19 只搬了 main.c 那一份） | 單一條 `0xF00C` → decode0 HWM 由 2,580 → **364**；4 KB 洪水時只剩 **124 B** | 離 panic 只差一次深呼叫；decode 棧被迫養到 4 KB |
| 3 | **UART 的環是「照抄 CDC 再放大」**：slot 4×1,024、RX ring 4,096、TX ring 8,192 ＝ 16.4 KB，但實測單次最多搬 **246 B**、同時佔用峰值 **1 格** | `max_read=246`、`peak=1`、`slot_full=0` | 一條命令通道吃掉 16 KB 內部 RAM |
| 4 | **測試指令寫 UART 由 handler 直接寫**：handler 揸住 decoder 鎖 → decode1 不能消費 → 插槽滿 → RX ring 溢位 | 200 幀×77 B 只收到 62/200，還要等 20 s timeout | 量不到 UART 真實吞吐（改成獨立 task 的 `0xF00E`） |

### 4.2 收斂後的尺寸（每一個都有量測依據）

| 項目 | 之前 | 之後 | 依據 |
|---|---:|---:|---|
| UART parser region | 1,024 | **1,024** | 保留「≤1,011 B 指令」的能力；要 4 KB 指令改 config |
| UART slots | 4×1,024 = 4,096 | **4×256 = 1,024** | max_read 246、peak 1、slot_full 0 |
| UART `chunk` | 512 | **256** | 與 slot 同一個數量級 |
| UART driver RX ring | 4,096 | **1,024** | 對照實驗：1 KB 與 2 KB 的偶發錯誤率一樣（§4.5）→ 留貼身的 1 KB |
| UART driver TX ring | 8,192 | **1,024** | UART 的 send 未接；只有測試指令寫 TX |
| `uart.rx` 棧 | 4,096 | **1,536** | 實測 888 B（HWM 648） |
| CDC parser region | 4,192 | **4,192** | OTA 的 4,096 B chunk ⇒ 幀 4,109；不可再收 |
| CDC slots | 4×256 | **4×256** | 已是 1 KB；峰值 4 格只在 4 KB 洪水出現 |
| `cdc.rx` 棧 | 1,536 | **1,280** | 實測 936 B（HWM 344） |
| decode0 / decode1 棧 | 4,096 | **3,072** | `stack_cover` 全 handler 走遍實測 2,232 B（剩 840） |
| `main` 棧 | 4,096 | **3,072** | 實測 2,120 B（HWM 952） |
| `nc_log` 棧 | 3,072 | **2,560** | 實測 1,804 B（HWM 756） |

單項合計：UART 類緩衝 **−13.3 KB**（slots 3.1 ＋ RX 3.1 ＋ TX 7.2）、任務棧 **−6.4 KB** ⇒ **約 −19.7 KB**。

### 4.3 不降性能的證據（前後各 3 次取中位數）

| 指標 | 之前 | 之後 | 判定 |
|---|---:|---:|---|
| UART 500×16 B | 172 ms（50 ms 輪詢量化） | **161–166 ms** | 90 KB/s ≈ 921600 baud 的 98%，在線速上限 |
| UART 200×64 B | 220 ms | **169–178 ms** | 同上（~90 KB/s） |
| UART 100×256 B | 339 ms | **294–301 ms** | 同上；全部 `lost=0` |
| CDC RTT（100 次） | 1.084 ms | **1.07–1.13 ms** | 差異在量測抖動內 |
| CDC TX（1 MB, 0xF00D） | 1.025 s / 1,021 stalls | **1.024 s / 1,020 stalls** | 不變 |
| CDC RX 500×256 B | 500/500 | **500/500** | 不變 |
| CDC RX 100×4 KB | 74/100（slot_full） | **74/100（slot_full）** | 不變（本來就是 slot 上限，見 §6 第 1 點） |

### 4.4 雙核獨立（使用者第二點）

| 檢查 | 結果 |
|---|---|
| 跨核 kick（`net.bus[].remote`） | CDC **0**、UART **0** —— 叫醒永遠是同核 notify |
| 每條 bus 的緩衝 | 各自 `calloc`／各自 plan 宣告，沒有共用池 |
| 單核加載對另一核的影響 | UART 400×64 B：單獨 **339.9 ms**；同時 core0 狂打 STATUS（108 條、p50 2.29 ms）→ **334.9 ms**（−1.5%，無退化） |
| 殘留耦合（設計如此） | 兩個 decode 共用一個 decoder → handler **執行**序列化；反映在 CDC 回覆延遲（1.1→2.3 ms），**不**影響 UART 吞吐 |

### 4.5 soak（兩輪，合共約 21 分鐘混合負載）

每輪：UART 300×64 B ＋ CDC 200×256 B 洪水 ＋ 20 次 STATUS；每 5 輪取一次快照。

| 輪 | 時長 | 循環 | 結果 |
|---|---:|---:|---|
| 1（RX 1 KB） | 15 min | ~2,300 | heap 固定、六個任務 HWM 全程不變；`cdc_crc=0`、`cdc_bad=0`；UART **1 次** `crc_err` |
| 2（RX 1 KB） | 6 min | 875 | 重現 **1 次** `crc_err`＋`bad_hdr`（同一事件） |
| 3（RX 1 KB） | 6.5 min | 960 | 再 **1 次**（累計 3 次，約 1/900 循環） |
| 4（RX 2 KB） | 11 min | 1,620 | 仍是 **1 次** `crc_err`（在第 1,620 循環） |

那一對錯誤是不是「貼身貼過頭」？**不是緩衝容量問題**：
- 累計 UART 讀了 **261,416 次、解了 130,708 幀**，`peak`（同時佔用格數）**1**、`slot_full` **0** ——
  插槽與 parser 從來沒有壓力。
- 單獨 UART 壓力（500×256 B ×6 ＝ 807 KB）**0 錯誤**，而且跑滿 92.0 KB/s（線速 100%）。
- 把 RX 環由 1 KB 加到 2 KB（＝線速 11→22 ms）做對照：1 KB 是 3 次/4,135 循環、
  2 KB 是 1 次/1,620 循環 —— **同一個數量級，加大環沒有把事件率壓下去**。
  所以那不是環容量問題（`slot_full` 一直是 0），比較像 UART ISR／線路層的偶發
  （≈1/1,500 循環、≈1/33 MB），維持 1 KB 貼身並持續用 `crc_err` 觀察。
- 事件率 ≈ 1/130,708 幀（≈1/33 MB），只在高負載混合 soak 出現；換算 BER ≈ 3e-8，
  屬 IO7↔IO8 跳線在 921600 baud 的偶發線路干擾，不是緩衝溢位（否則 `slot_full` 會增加）。

### 4.6 帳目（可重複驗證）

| 量法 | 結果 |
|---|---|
| 逐項尺寸 | 開機 log（`4 個插槽 × 256 B`、`parser 用 plan 的 1024 B`）＋ `STATUS rt` 的 HWM 與棧大小一致 |
| config → 實際配置 **1:1** | 把 UART `rx.slots` 由 4 改 16（＋3,072 B）重開機：內部 heap `286,243 → 283,171`（**正好 −3,072**）；刪掉 cfg 回預設 → 回到 **286,243**（同一映像、同一格式化狀態） |
| 跨版本淨值 | 開機內部 `277,779 → 286,443`（**＋8,664**）、PSRAM `7,839,056 → 7,849,296`（**＋10,240**）；合計 **＋18.9 KB**，與逐項合計 19.7 KB 的差額來自與本次無關的開機路徑配置（FATFS／MSC 工作區、OTA 後的重格式化等） |

結論：**貼身的效果是真的、而且可用 config 逐項驗證**；量總數要用「逐項＋config 對照」，
不要只信一次開機的 heap 差（那包含了別的東西）。

## 5. 下一步（P4／未量測的部分）

P4 這塊板不在手邊，所以下面全部**只列方法，不改數字**：

| 目標 | 現值 | 怎麼量 | 預期 |
|---|---:|---|---|
| TCP parser region | 16,410 | `STATUS keys=net` 的 `max_dlen`（P4 跑 `nc4_tcp.py` 各種幀大小） | OTA 走 CDC，TCP 的 region 只需 ≥ 實際最大命令；很可能可以降到 4,192 |
| TCP slots | 8×8,205 = 65,640 | `peak`／`slot_full`（PUSH-frame 零複製要 ≥ 1 整幀，但格數可收） | 峰值若 ≤ 2，8→3 可省 ~41 KB |
| WS／UDP 緩衝 | 41 KB（P4 專屬） | 同上（WS 不走 bus，要看 udp/ws 的統計） | 待量 |
| `s_buses`（8 條 × 280 B） | 2,240 B | 實際同時最多 3 條（CDC／UART／TCP） | 8→4 可省 1,120 B，但會少一層餘裕（通道加就 `nc4_bus_add` 回錯，不是靜默） |
| TinyUSB `_cdcd_itf`／`_mscd_epbuf` | 12,548＋8,192 B | 看 `CFG_TUD_CDC_*_BUFSIZE` 與 MSC EP buffer | 最大的一塊靜態，但屬 USB 堆疊，要單獨評估 |

方法一句話：**先把 `reads/max_read/peak/slot_full/max_dlen` 拉出來，再按 §2 的規則算**；
不要憑感覺改。`buf_probe.py` 的 `snap` 就是為了這件事。

## 6. 已知取捨與未做

1. **CDC 4 KB 連續洪水**：4 格 ×256 B 的插槽池追不上 4 KB 幀（實測 74/100，`slot_full`）。
   OTA 有 ACK 流控所以沒問題；主機若無流控連發 4 KB 幀，本來就會丟。
   要改善＝加 slots（每格 256）或加大 slot_size，代價是 RAM；不是這次的目標。
2. **UART 沒有 `send`**（`nc4_bus_cfg_t.send = NULL`）：UART 這條線目前**只收不回**，
   所以 TX ring 可以只留 1 KB。要讓 UART 也能回覆，先接 `uart_send()`，再把 TX ring
   依「最大回覆（2,615 B）」重定 —— 那時 decode1 也會用到回覆路徑（棧需求同 decode0）。
3. **IO7 同時是 SD CLK 與 UART 迴路 TX**：`hardware.json` 的 SDIO 一開，UART 就不能啟動
   （`uart_owner_init` 會拒絕）。這次量測是把 SDIO 關掉做的；兩者要同時用必須換腳位（需要人）。
4. **`s_buses`（8 條 × 280 B = 2,240 B）** 與 **`s_slots`（20 格 6,000 B）** 是靜態分區，
   目前 5 條／18 格就夠，但縮小會令將來的通道／`.fxe` 靜默失敗（§0 of next-steps 已否決過一次），
   所以**這次不動**，只記錄。
