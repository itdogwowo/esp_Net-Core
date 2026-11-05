# 分段寫入（FILE_BEGIN / CHUNK / END）可行性

> **狀態**：文件 + 實測（2026-11-02）。**尚未實作**，這份是動手前的驗證。
> **設計來源**：[architecture-flow.md](architecture-flow.md) §3、[roadmap.md](../00_pm/roadmap.md) §1
> **參考實作**：mp_Net-Core `slave/lib/sys/fs_manager.py`（`begin_write` / `_finalize_atomic_write`）
> **相關**：[fat-ownership.md](fat-ownership.md)（誰擁有 FAT）、[commands.md](../01_protocol/commands.md)

---

## 1. 文件已經定的東西

`architecture-flow.md` §3：

```text
FILE_BEGIN(name, size, crc) → 開 staging
FILE_CHUNK(offset, data)    → 寫 + 更新 crc
FILE_END                    → verify crc → validate → atomic rename → asset_version++
                              → ASSET_RELOAD
```

`roadmap.md` §1 已定案：**staging → CRC → validate → atomic rename → ASSET_RELOAD**；
§4 驗收條件是「斷電 20 次，要麼舊版完整，要麼新版完整，不能半寫入」。

## 2. mp 版實際做法（照抄得到的部分）

```text
FILE_BEGIN  → 開 <path>.tmp 寫入（不是寫入目標檔）
              記 file_id / total_size / sha256
              若 .tmp 已存在且身分相符 → 斷點續傳（從現有大小繼續）
FILE_CHUNK  → 依 offset 寫入 .tmp，每包回 0x2004 ACK {file_id, offset}
FILE_END    → 重讀 .tmp 算 sha256
              ├ 不符 → 刪 .tmp、回 0x2010 SHA_MISMATCH
              └ 相符 →
                   舊檔存在：舊 → .bak、.tmp → 目標、記 pending
                   沒有舊檔：.tmp → 目標
                 回 0x2006 {exists, sha, size, path, free, pending}
FILE_CONFIRM → 刪 .bak、清 pending
FILE_UNDO    → 刪新檔、.bak 改回原名
```

三個值得照抄的點：

1. **先寫 `.tmp`**：任何時候斷電，目標檔都是完整的舊版。
2. **每包 ACK**：client 不需要猜，出錯即刻停。
3. **pending 旗標**：讓上層知道「有新檔但未確認」。

C 版**不需要** manifest（MP 那份是給 /sd + /ram 雙卷用的），也不需要 delta 落盤。

## 3. 實測：可行，但有四條界線

### 3.1 空間預算（最大的限制）

| 項目 | 實測 |
|---|---|
| `data` 分區 | 6,824 KiB（6.66 MB） |
| 已用 | 5,136 KiB（其中 `big.bin` 佔 5,120 KiB） |
| 可用 | **1,688 KiB** |
| cluster | 4 KiB（WL sector = 4 KiB，`fs->csize=1`、`fs->ssize=4096`） |

寫入時需要的空間：

| 情境 | 需要 | 可用 1,688 KiB 時的上限 |
|---|---|---|
| 新檔 | 1× 檔案大小 | **1.6 MB** |
| 覆蓋既有檔（大小 S） | **2× S**（`.tmp` + 舊檔，直到 rename） | **844 KiB** |
| 覆蓋 + 保留 `.bak` | 2× S（`.bak` 在 confirm 前仍在） | 同上 |

> ⚠️ 現在的 1,688 KiB 是被 `big.bin`（5 MB 測試檔）吃掉的。實用時應該先刪掉它，
> 可用空間會回到約 6.7 MB，上限相應變成：新檔 6.7 MB / 覆蓋 3.3 MB。

### 3.2 速度

| 測量 | 數值 | 來源 |
|---|---|---|
| 裝置擦除 4K | 38.6 ms/op | `0xF001` mode 2（該 mode 已移除，數字保留作歷史） |
| 裝置寫 4K（raw） | 709 µs/op | `0xF001` mode 3（該 mode 已移除，數字保留作歷史） |
| **NC4 CDC 寫入實測** | **141.6 KB/s** | OTA（2.5 秒推 359,568 B） |
| MSC（主機拖檔） | 75 KB/s | 實測 |

推論：透過 FatFS 寫（多了 FAT 表與目錄更新）大約 **100–140 KB/s**。
1 MB ≈ 8–10 秒；100 KB ≈ 1 秒。

**所以「接管 FAT 的時間」就是 8–10 秒 / MB** —— 這段時間主機看不到碟。這是可接受的，
但 timeout 與取消一定要有。

### 3.3 斷電 / 中斷

| 情況 | 結果 | 恢復方式 |
|---|---|---|
| 寫 `.tmp` 期間斷電 | 目標檔完好（舊版），`.tmp` 留下半截 | 開機掃 `*.tmp` 刪掉（或按 offset + sha 續傳） |
| rename 中途斷電（舊→`.bak` 完成、`.tmp`→目標未完成） | 目標檔不存在、`.bak` 存在 | 開機見到 `.bak` 而沒有目標 → 改回原名 |
| 正常完成 | 新版完整、`.bak` 留著 | `FILE_CONFIRM` 刪 `.bak` |

FatFS 的 rename 是同目錄的目錄項改寫（一個 sector），配合上面的開機檢查，
可以做到「要麼舊版完整、要麼新版完整」。

### 3.4 協定面的容量

| 限制 | 值 | 影響 |
|---|---|---|
| NC4 payload 上限 | 8,192 B | `FILE_CHUNK` = file_id(2) + offset(4) + data → data ≤ **8,186 B** |
| 每包 ACK | 0x2004 | 8 KB/包、100–140 KB/s → 每包 ~58 ms；RTT 佔比 < 10%，**不需要 pipeline** |
| CDC RX 緩衝 | plan 給 `nc4.rx` 8,205 B | 剛好夠一幀；與 OTA 相同（OTA 用 4 KB 更保守） |

---

## 4. 結論

**可行。** 沒有硬件或協定上的阻礙；限制全部是「空間 × 速度 × 要正確處理中斷」。

動手時必須一起做的四件事：

1. `FILE_BEGIN` 先檢查 `free ≥ total_size`（覆蓋要 `2 × total_size`），不足回 `err_no_space`。
2. 寫入前自動 `file_mode=1`；`FILE_END` / 錯誤 / timeout 一律還原 `file_mode=0`。
3. 閒置 timeout（建議 5 秒沒有 CHUNK 就 abort），並支援 `STATUS_SET file_mode=0` 當取消。
4. 開機清理：`*.tmp` 刪掉；`*.bak` 而目標不存在 → 還原。

**已修掉的相關 bug**：`free_bytes()` 原本用 512 當磁區大小，把可用空間少報 8 倍
（211 KiB vs 真正的 1,688 KiB）。已改成 `fs->csize * fs->ssize`，並與主機 `df` 對過。

---

## 5. 還沒決定的

- **要不要保留 `.bak`**：MP 有 `FILE_CONFIRM`/`FILE_UNDO`。C 版若不需要回滾上一版，
  可以 rename 後直接刪 `.bak`，省一半空間（覆蓋上限從 free/2 變 free）。
- **要不要斷點續傳**：`.tmp` + offset + sha 已經足夠，但要決定「重新 FILE_BEGIN 時
  遇到同一個 `.tmp` 是續傳還是重來」。
- **CRC vs SHA256**：文件寫 CRC，schema 寫 `sha256:bytes_fixed`。以 schema 為準（SHA-256）。
