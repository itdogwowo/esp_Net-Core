# Pixel 緩衝環與任務協同（現況＋待拍板）

> 2026-11-25。四層四任務：
> `pixel_effect_task` → `pixel_paint_task` → `pixel_encode_task` → `render_task`。
> 這頁只講「四層之間用什麼 buffer、怎樣 handoff、滿了怎樣」；量測數字以
> [status.md](../00_pm/status.md) 為準。

## 1. 一張幀走過的四段

```text
effect pool（2 塊，PSRAM）
  │  nc_pipe_effect_frame_t：seq／plan_gen／naprog／nv[]／vals*[]（只傳指標）
  │  paint 完成一幀 → ack(seq)；effect 之後才重用該 slot
  ▼
pixel_paint_task
  │  直接寫環槽 payload（零拷貝）
  │  nc_px_ring_begin_ex() → layout apply／dither／white mix → nc_px_ring_commit()
  ▼
prd 環（N 格；每格 64 B 前綴（24 B 表頭＋padding）＋px×4 B）
  │  encode 用 peek 取「最新」一格；舊 READY 格會被釋放（假消費）
  ▼
pixel_encode_task
  │  後端 acquire（拿環格 + DMA 槽）→ encode → 入「格索引」隊列
  ▼
render_task push loop
  │  queue → submit；DMA 完成 → reclaim → nc_px_ring_release(seq)
  ▼
硬件（I80／SPI／RMT／PARLIO）
```

## 2. 各段的實際契約

### 2.1 effect ↔ paint（值流，不是環）

- effect 有 **2 塊 pool** 交替寫；paint 只拿 `nc_pipe_effect_frame_t` 的**指標**。
- paint 完成整幀（成功寫環）後 `nc_pipe_effect_ack(seq)`；effect 的 `wait_slot()`
  見到舊 slot 未 ack 就 `vTaskDelay(1)` 等，**不會覆寫正在讀的值流**。
- 代價：paint 慢（環滿）→ effect 等；effect 的 50 fps 會掉，但唔會撕裂。

### 2.2 paint → 環

- 環 `nc_pixel_q_t`：`slots` 格（預設 3；2,400 px 4 子幀用 5）、
  每格 `state = FREE／FILLING／READY`、`wr`、`seq`、`busy`。
- `begin_ex()` 只拿 FREE 格；**滿了回 NULL，唔覆蓋**，paint `busy++` 下一輪再試。
- `commit()` 寫 `hdr->seq`、`seq++`、state READY、`wr++`。
- 零拷貝：`layout apply` 直接寫 `slot_payload`；冇 canvas、冇複製。
- 4 子幀：**一個視覺幀 commit `ns` 次**（每個子幀一個 seq／一格）。

### 2.3 環 → encode（兩種取格政策，2026-11-26 起）

- **多子幀（`CONFIG_NETCORE_PIXEL_SUBFRAMES > 1`）→ 逐格 FIFO**：後端 `acquire()` 用
  `nc_px_ring_next(after_seq)`，拿 **seq 最小而且 > 上次已交** 的那格；
  **唔會假消費**其他格。四個子幀一個一個依次送去 DMA；環滿就背壓 paint
  （`begin()` 回 NULL → paint busy++），唔會漏。
- **單子幀 → 最新優先（mailbox）**：仍然用 `nc_px_ring_peek()`，拿**最新**一格，
  `peek` 會把「比最新舊的 READY 格」釋放（假消費），encode 記 `skipped`。
- 兩者由 `nc_px_ring_take()` 依 `CONFIG_NETCORE_PIXEL_SUBFRAMES` 揀，後端唔自己判斷。
- 為什麼要分開：4 子幀抖動（12→10 bit）要四個子幀**全部**送到，只交最後一個等於冇抖動；
  單子幀時舊幀冇意義，掉舊格換低延遲反而正確。
- 後端自己的 DMA 槽（i80：`slots` 個 internal buffer）滿了就這輪不拿。
- encode 完成 → `px_ready_t{lane,slot,seq}` 入隊（8 格／lane 的索引，不 copy 像素）。

### 2.4 encode → render

- queue 容量 `NC_PX_MAX_LANES * 2`（16 項）；push loop 每 10 ms 收一次。
- push 先 `reclaim()`：只有 DMA 完成才 `nc_px_ring_release(seq)`；
  然後 `submit()`。
- `submit` 失敗 → 還格 + `fail++`；queue 滿 → 直接 submit fallback（唔放生 DMA 槽）。

## 3. 三個「緩衝」角色（唔好混淆）

| Buffer | 誰寫 | 誰讀 | 大小／數量 | 滿了怎樣 |
|---|---|---|---|---|
| effect pool | effect | paint | 2 × Σ nv × 2 B（PSRAM） | effect 等 ack（背壓） |
| prd 環 | paint | encode（多子幀 FIFO／單子幀 peek 最新） | N ×（64 B＋px×4） | paint 重試（busy++，背壓） |
| DMA 槽 | encode | 硬件 DMA | N × units（internal） | acquire 這輪不拿 |
| ready queue | encode | push | 16 × 12 B | fallback submit |

## 4. 現況的張力（要討論）

1. **最新優先 vs 每幀必送**（2026-11-26 已定案）：**多子幀用 FIFO**（每幀必送、背壓）、
   **單子幀用 mailbox**（最新優先、丟舊格）。之前一律用 `peek`（最新優先），所以 4 子幀
   只有最後一個送到 —— 見下面 §4.2 的實測。
2. **4 子幀不是原子 group**：paint 連續 commit 4 格；如果 encode 的 tick
   刚好夾在中間，可能只 encode 到 1–3 個子幀，下一個 tick 才補。視覺上係「半個
   抖動 frame」，通常看不出，但語義上唔完整。
   > **2026-11-26 已修**：`peek` 每次都會把「除最新以外、所有 READY 格」直接還原成 FREE
   > （假消費），而 paint 係喺同一次 `content_frame_once()` 內連續 commit 完 `ns` 格 ——
   > 所以舊行為下 encode 每 tick 只拿到最新 1 個，另外 3 個喺下一次 peek 就被釋放，
   > **永遠唔會補送**（`skipped ≈ frames×3`）。
   >
   > 修法（已入 code，host test 覆蓋、未上板量）：
   > ① `nc_px_ring_next()` 逐格取（FIFO、唔假消費）＋`nc_px_ring_take()` 依
   > `CONFIG_NETCORE_PIXEL_SUBFRAMES` 揀政策；
   > ② paint 的 `for (sf…)` 加 **續傳**（`s_sf_done`）：環滿停手之後由下一個子幀續，
   > 唔會由 `sf=0` 重交（否則同一個子幀重覆入環，抖動相位亂）。
   > 驗法：`0xF061` 的 `skipped` 應該係 **0**（4 子幀模式）；paint 會有 busy++（背壓）。

3. **三種滿了策略唔一致**：effect 等 ack、paint 重試、encode 跳過、queue fallback。
   冇一個統一的「丟幀／背壓」政策。
4. **多後端冇 fanout**：paint 只寫 `nc_render_first_ring()`。I80 的多 lane 共用
   同一條環（OK）；但如果有兩條獨立環（例如 i80 + mock），第二條永遠冇畫面。
5. **節拍主人**：render 建 timer；encode 等它的 tick；paint／effect 各自 free-run。
   render 被 config 關 → encode 停；encode 被關 → ring 會滿 → paint 停。
6. **環深度**：預設 3；4 子幀要 5（project 已設）；但 Kconfig 只寫「建議」，
   冇 boot-time 檢查，配置錯會直接掉幀。
7. **cache／msync**：環在 PSRAM，DMA 讀的是 internal 的 DMA 槽（encode 先 copy／
   轉置），所以環本身不需要 msync；舊文件寫「DMA 讀取前會 msync」係講 DMA 槽。

## 5. 可選方案

### A. 保持 mailbox（最新優先）

- 環＝「最新畫面信箱」；滿了 paint 等，encode 永遠追最新。
- 優點：延遲最低、程式最簡單；缺點：負載重時丟幀（`skipped`），FPS 不保證。

### B. 改成 FIFO 佇列（每幀必送）

- 環變成 N 格 FIFO，encode 逐格取；滿了 paint 等。
- 優點：每幀都送到；缺點：延遲累積，舊畫面可能遲到；要加 drop 政策。

### C. 混合（建議討論方向）

- effect → paint：保持 2 塊 pool＋ack（背壓）。
- paint → 環：保持 mailbox，但**以視覺幀為單位 commit**（4 子幀一次 group）；
  encode 只拿完整 group。
- encode → render：保持 index queue（FIFO，因為 DMA 要順序）。
- 多後端：每個 backend 一條環，paint 按 lane 清單 fanout（或 render 提供 ring list）。
- boot-time 檢查 `ring_slots >= subframes + 1`，不足就明示失敗。

## 6. 要拍板的問題

1. 畫面哲學：**永遠最新**（丟舊幀）定**每幀必送**（背壓、延遲）？
2. 4 子幀要唔要 atomic group（一個 visual frame 一次 commit／一次 encode）？
3. 多後端（I80＋mock／LCD）要唔要 fanout？要的話，一條環定每後端一條？
4. 環滿：paint 等（現在）／丟最舊／覆蓋最舊？邊個可以接受？
5. 節拍主人：render timer（現在）定 encode／paint 自己？
6. effect→paint 的 ack 要唔要 timeout／丟幀 policy（現在無限等）？
7. 環深度：固定 3／5，定按 `subframes` 自動計？
