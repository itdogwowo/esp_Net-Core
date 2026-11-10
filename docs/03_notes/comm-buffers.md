# 通訊緩衝（最終定案）

> 2026-11-09。範圍：**bus frame 的收、解、轉、發**。
> 像素／音效 buffer 是另一條線（[psram-surface.md](psram-surface.md)），不要混在一起。
>
> 這頁取代 `buffer-pool.md` §3 的臨時 forward queue；`nc_hub` 是新工具，
> 用它做轉發的跨核 handoff。

## 1. 定案摘要（先看這張）

| 用途 | 放哪 | 機制 | 為什麼 |
|---|---|---|---|
| RX parser／PUSH slots | **internal** | per-link（現況） | 熱路徑、framing 狀態、低延遲 |
| 大幀 staging（OTA／FILE） | **PSRAM**（可選） | `nc_pool.bulk`（預設關） | 大而冷；需要才開 |
| 一般命令 staging | **internal**（可選） | `nc_pool.small`（預設關） | 尚未接 header-first；先唔浪費 |
| 回覆幀 | **PSRAM** | per-core 2,615 B | 已有；一核一份 |
| **跨核／同核轉發** | **PSRAM** | **`nc_hub` per（目的 link × 來源核）** | 可慢；嚴格 FIFO；generation／統計 |
| TX driver 環 | 各 transport | UART／CDC／TCP／SPI | 硬體限制，不能共用 |

**一句話**：**RX 留在 internal；轉發全部走 PSRAM 的 `nc_hub`；`nc_pool.forward` 取消。**
轉發慢一點（+0.2–0.4 ms/4 KB）換取：
- 來源核**不再被目的鏈阻塞**；
- 每個目的 link 有自己的 FIFO（唔會被慢 UART 塞住其他 link）；
- `nc_hub` 的 generation／core／violations 可以直接看。

## 2. 架構

```
link A（core X）                       link B（core Y）
  parser／region                         decode loop
      │                                      │
      │ parser_forward()                     │ nc4_fwd_poll(core Y)
      ▼                                      ▼
  ┌─────────────────────────────────────────────────┐
  │ fwd hub[B][X]  （PSRAM，SPSC，strict FIFO）       │
  │  slot = [hdr 8 B][frame ≤ max_frame]             │
  │  writer = core X 的 task（有 mutex）              │
  │  reader = core Y 的 decode loop                   │
  └─────────────────────────────────────────────────┘
      │
      └──► nc4_link_send(B, frame) → B 的 TX 環
```

- **每個（目的 link, 來源核）一條 hub**：S3 最多 8 links × 2 核 = **16 條**；
  P4 4 links × 2 = **8 條**。
- **同一條 hub 只有一個 writer（來源核）＋一個 reader（目的核）**；
  來源核如果有多個 task（decode／httpd／udp）會寫同一條，**用 mutex 序列化 writer**
  （不是 spinlock：4 KB PSRAM copy 0.2 ms，唔可以關中斷）。
- **同核轉發也走 hub**：來源 task 寫、同核 decode loop 讀；多一次 PSRAM round trip，
  但換來「來源永遠唔會等目的 TX」。這是這次定案的核心取捨。
- **唔用 per-direction 一條 hub**：那會讓慢 UART head-of-line 塞住同核的 TCP；
  per-link 貴一點 PSRAM，但係 router 應該要的。
- **reader 在目的核的 decode loop** 每輪 drain；`nc_hub_read_begin` 非阻塞。
  每輪每條 hub 最多送 **4 幀**（budget），避免轉寄把 RX 餓死。
  如果 `nc4_link_send` 阻塞（UART 環滿），只阻塞目的核，不影響來源。

## 3. 封包格式

```c
typedef struct {
    uint16_t len;      // frame bytes（≤ max_frame）
    uint8_t  src;      // 來源 link index（統計／除錯）
    uint8_t  dst;      // 目的 link index（與 hub 對應，冗余但好查）
    uint32_t seq;      // 來源核的單調序號（除錯／gap 偵測）
} nc_fwd_hdr_t;        // 8 B
```

- slot = `align64(sizeof(hdr) + max_frame)`：
  - S3 `max_frame=4,109` → **4,160 B**
  - P4 `max_frame=8,205` → **8,256 B**
- frame 內容係已驗 CRC 的完整 NC4 幀（header＋payload＋CRC）。
- `nc_hub` 的 generation 由 commit 自動遞增；hdr.seq 係另一層（跨 hub 對帳用）。

## 4. 設定

```json
"nc4": {
  "forward": {
    "enable": 1,
    "where": "psram",
    "depth": 2,
    "max_frame": 4109
  },
  "pool": [
    { "name": "small", "size": 1024, "count": 8, "where": "internal", "enable": 0 },
    { "name": "bulk",  "size": 4112, "count": 3, "where": "psram",    "enable": 0 }
  ]
}
```

- `where`: `psram`（預設）／`internal`（只給測試；唔建議）。
- `depth`: 2（預設）；burst 多就 4。每多一格多 `slot × hubs` PSRAM。
- `max_frame`: S3 4,109；P4 8,205。大過就 drop（parser 已經有上限）。
- `pool` 預設 **enable:0**：等 header-first／OTA 真正用時才開；現時唔佔 internal。
- **forward 唔再放 `nc4.pool`**；舊的 `forward` class 保留 enum 但 config 不再建。

## 5. 記憶體

### S3（8 links、2 核、depth 2）

| 項目 | internal | PSRAM |
|---|---:|---:|
| forward hubs 16 × 2 × 4,160 | 0 | **133 KB** |
| 回覆幀 2 × 2,615 | 0 | 5.2 KB |
| `nc_pool` small／bulk | **0**（預設關） | 0 |
| RX parser／PUSH slots | 現況（per-link） | 0 |
| **合計** | **0 新增** | **~138 KB** |

- 對比之前的 `nc_pool` forward 2×4,160 internal＋small/bulk 20.7 KB：
  **internal 反而省 ~29 KB**（pool 預設關）＋8.2 KB（forward 移走）。
- PSRAM 138 KB＝7.85 MB 的 **1.8%**。

### P4（4 links、2 核、depth 2）

- 8 hubs × 2 × 8,256 = **132 KB PSRAM**；回覆 5.2 KB。
- internal 新增 0；32 MB PSRAM 幾乎全空。

### 成本

- 4 KB frame：PSRAM 寫 ~0.2 ms＋讀 ~0.2 ms → 轉發延遲 **+0.4 ms**。
- 8 KB frame：**+0.8 ms**。
- 來源核只付「寫」的 0.2–0.4 ms（copy＋mutex），唔等目的鏈。
- 轉發吞吐上限：PSRAM 21 MB/s ÷ 4 KB ≈ **5,000 幀/s**（理論；實際受 hub depth／TX 限制）。
- 對比其他 bus：UART 921600 的 4 KB 要 44 ms；轉發不是瓶頸。

## 6. 失敗與限制

| 情況 | 行為 |
|---|---|
| hub 滿（writer `write_begin` NULL） | **drop＋`drop_full` 計數**；不重試、不阻塞 |
| frame > `max_frame` | drop＋`drop_big`（正常不應發生） |
| 目的核 `nc4_link_send` 失敗 | drop＋`drop_send`（來源 buffer 已 release） |
| 目的 link 冇 route／core = -1 | 開機 fail-visible；該 hub 不建立 |
| hub registry 滿（32） | `nc4_fwd_init` 回錯誤；**不靜默** |
| 同一條 hub 多 writer | writer mutex 序列化；reader 只有一個 |
| 來源 task 在 ISR 呼叫 | **不支援**；forward 只在 parser dispatch（task） |
| PSRAM 與 flash 同時 | flash 停 cache 時 PSRAM 一齊停；轉發會等，屬已知 |
| 跨來源順序 | 每條 hub FIFO；兩個來源核到同一 link 係兩條 hub，**跨來源不保證順序** |

## 7. 統計

`STATUS keys=net`：

```json
"net": {
  "fwd": {
    "enabled": 1, "where": "psram", "hubs": 16, "depth": 2, "slot": 4160,
    "frames": 0, "drop_full": 0, "drop_send": 0, "drop_big": 0,
    "max_fill": 0, "violations": 0
  }
}
```

- 每條 hub 的 `fill/gen/wc/rc/viol` 已經在 `mem.hub`（`nc_hub_registry_json`）。
- `nc_hub_stats()` 可逐條拿 `publishes`／`generation`／`writer_core`／`reader_core`。

## 8. 與現況的差異（migration）

| 階段 | 內容 | 驗收 |
|---|---|---|
| **P0** | 這頁定案；`system.json` 加 `nc4.forward`；`pool` 預設關 | 文件／config 可讀 |
| **P1** | `nc4_fwd` 改 hub 版：per（dst link × src core）、PSRAM、writer mutex、reader 在 decode loop | build＋hosttest；舊 queue 保留 `mode=pool` 做 A/B |
| **P2** | 預設 `mode=hub`；STATUS `net.fwd`；上板量 latency／drop | CDC→UART／P4→TCP 轉發 |
| **P3** | 移除 `nc_pool.forward`／舊 queue；tap／refcount（用 `nc_hub_generation`） | 無回歸 |
| **P4** | P4 8,205／8,256、4 links；必要時 depth 4 | P4 上板 |

- **P1 之前**：現有 `nc_pool.forward` 2×4,160 internal＋`nc4_fwd` queue 仍然可用；
  這次只改文件與 config，不動 code。
- **P2 之後**：`nc_pool` 只保留 small／bulk（預設關），forward class 不再使用。

## 9. 為什麼不是其他方案

| 方案 | 否決原因 |
|---|---|
| per-direction 2 條 hub | 慢 link head-of-line 塞住同核其他 link |
| 每條 link 一條 hub（唔分來源核） | 兩個核可以同時 forward → 2 writers，違反 `nc_hub` SPSC |
| forward 留在 internal `nc_pool` | 使用者明確接受 PSRAM；internal 留給 RX／stack |
| 同步 `nc4_link_send`（現況） | 來源核會被慢 TX 阻塞（UART 4 KB = 44 ms） |
| 每幀 malloc | 碎片、時間不確定；hub slot 開機固定 |

## 10. 未定／下一步

1. `max_frame` 要唔要 per-link（現在全域）？先用全域，量到某條線特別大再拆。
2. `depth` 2 vs 4：先用 2，量 `drop_full` 再調。
3. 跨來源順序：現在不保證；如果應用需要，就要 per-source-link hub（更多 PSRAM）。
4. tap／第三方讀取：用 `nc_hub_generation` 防 ABA，另開 tap hub 或 refcount；未做。
5. P4 8K：slot 8,256、depth 2；上板量 PSRAM 頻寬與 TCP 轉發。
6. **實作 P1**：`nc4_fwd` hub 版（保留舊 queue A/B）。這是最後一塊。
