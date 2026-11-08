# 量測方法：貼身化與「不降性能」的驗收

> **這份是流程，不是紀錄。** 下一次要調**任何**緩衝、任務棧、優先級、通道參數時照這份做。
> 這次的實測紀錄在 [buffer-tightening.md](../03_notes/buffer-tightening.md)。
>
> **適用範圍**：link 緩衝（parser／slots／driver ring）、任務棧、靜態分區上限、
> 優先級與同核干擾。也適用於任何「改小之後要證明沒變慢」的改動。
>
> **前置**：板子在線（`tools/nc4.py --port … status sys` 有回應）、
> 量測用的**測試映像**（`python3 make.py build s3 --tests`；`0xF00x` 指令只在測試映像存在）。

> **TL;DR（四條命令）**
> ```bash
> python3 make.py build s3 --tests                     # 量測要用測試映像
> tools/bench/buf_probe.py --port /dev/cu.usbmodem* session --label before --out /tmp/before.json \
>        --uart 500:16 --uart 200:64 --storm 200:256
> #   …改 config／尺寸 → REBOOT（只改 config 不用燒片）…
> tools/bench/buf_probe.py --port /dev/cu.usbmodem* session --label after --out /tmp/after.json \
>        --uart 500:16 --uart 200:64 --storm 200:256
> tools/bench/buf_probe.py compare --before /tmp/before.json --after /tmp/after.json
> ```
> `compare` 的「退步項」必須是 0 才算過關；不是 0 就看是哪一項，回退那一項再重跑。

---

## 0. 三條原則

1. **先定判準，再動手。** 每一項都要能回答「為甚麼是這個大小」；退步門檻先寫死
   （本專案：吞吐／延遲 > **3%** 就算退步；`lost`／`slot_full`／`crc_err` 只要增加就算）。
2. **一次只改一個變量。** 能改 config 就不燒片；要燒片就一次帶一組，並且先有 before 的
   同一顆映像（見 §6 陷阱 5：跨版本的 heap 不能直接比）。
3. **貼身 ≠ 最小。** 貼身 = **實測峰值 ＋ 明確邊際**。邊際要寫出來（例如「棧 ≥ 用量 ×1.25」、
   「環 ≥ 2 個 tick」），而不是「看起來夠」。

---

## 1. 七步流程（S0–S6）

| 步 | 做什麼 | 產出 | 需要板子 |
|---|---|---|---|
| **S0 盤點** | 從 ELF ＋原始碼列出每個緩衝：大小、class（static／plan／heap／stack）、擁有者、核心 | 一張表（§7 模板） | ❌ |
| **S1 儀表** | 確認 `STATUS keys=net` 有 `max_read / peak / slot_full / max_dlen`；`keys=rt` 有 HWM | 儀表清單 | ✅ |
| **S2 基線** | `buf_probe.py session --label before --out before.json` | JSON ＋人看的表 | ✅ |
| **S3 找異常** | 負載階梯（小／中／大幀）→ 看 `sizing` 的「建議 vs 現在」 | 差距清單 | ✅ |
| **S4 收斂** | 按 §3 公式定新值（config 先行，code 預設其後） | 新尺寸 ＋ 理由 | ✅ |
| **S5 A/B** | 同一個 `session` 再跑一次 → `compare before.json after.json` | 退步項 = 0 | ✅ |
| **S6 追偶發** | 有 `crc_err`／`slot_full` 才做；照 §5 的排除法 | 根因或「已排除」 | ✅ |
| **S7 文件化** | §7 模板 ＋ 決策紀錄（為甚麼選這個值、回退條件） | 文件／commit | ❌ |

---

## 2. 工具：每個子命令只回答一個問題

全部在 [tools/bench/buf_probe.py](../../tools/bench/buf_probe.py)。

| 子命令 | 回答什麼 | 典型用法 |
|---|---|---|
| `snap` | 現在裝置的完整狀態（sys/net/rt/mem/log/storage/cfg） | `--port … snap` |
| `sizing` | 依峰值換算「建議尺寸」 | `--port … sizing --declare CDC=4096` |
| `session` | 跑一遍**標準量測**（reset → rtt → UART 階梯 → CDC TX → 洪水 → 峰值） | `--port … session --label before --out before.json --uart 500:16 --uart 200:64 --storm 200:256` |
| `compare` | 兩個 session 並排，>3% 退步標 ⚠ | `compare --before a.json --after b.json` |
| `rtt` | 指令往返延遲（p50/p95/max） | `--port … rtt --n 100` |
| `uart` | UART 管線吞吐（`0xF00E` 獨立 task 灌流） | `--port … uart --frames 500 --size 256` |
| `txbench` | 裝置→主機 CDC 吞吐（`0xF00D`） | `--port … txbench --bytes 1048576` |
| `storm` | 主機→裝置連發（`0xF0FE` 不回覆，`0xF00C` 取進度） | `--port … storm --n 500 --size 256` |
| `idle` | 對照組 | `--port … idle --seconds 60` |

`sizing` 的 `--declare` 很重要：**只靠觀察到的流量會把 region 算得比真實需求細**
（例：CDC 這次只收到 5 B 的命令，但 OTA 要收 4,096 B → `--declare CDC=4096`）。

---

## 3. 尺寸公式（先算，再量，再定案）

| 項目 | 下限 | 邊際規則 |
|---|---|---|
| parser region | `13 + max_payload`（9 B 表頭 ＋ 4 B CRC） | 要「連續大幀不重同步」再 **+ chunk−1**；否則 OTA 那種有流控的場景不用 |
| `slot_size` | 實測 `max_read` | round up 到 128；≤ region |
| `slots` | 實測 `peak + 1` | `slot_full > 0` 就是不足（不是「大概」） |
| `chunk` | 與 slot 同一個數量級 | 越大每輪越少 syscall，但也越大暫存 |
| driver ring | 2 × tick（HZ=100 → 20 ms 線速量） | 同核任務可能壓住 producer 一個 tick（§4） |
| 任務棧 | 實測 used | **≥ used × 1.25 且 ≥ used + 512 B**；用 `stack_cover.py` 走遍全部 handler 後才定 |
| 靜態陣列上限 | 實測同時用量 | 留 2–3×；滿了要回錯並印帳，不能靜默 |

---

## 4. 雙核／同核隔離的驗收

| 檢查 | 合格條件 | 怎麼看 |
|---|---|---|
| 跨核叫醒 | `net.bus[].remote == 0` | `STATUS keys=net` |
| 一核滿載不拖累另一核 | 另一核的吞吐／延遲變化 ≤ 5% | 先單獨量，再同時量（`session` ＋另一條通道的負載） |
| 同核優先級 | **租戶任務不得與 producer 同級** | 同級 → FreeRTOS 10 ms 時間切片 → 偶發溢位（實測：CRC 率 ×3.5） |
| 共用鎖 | 共用一個 decoder 時，handler **執行**序列化是設計；要量的是它對延遲的影響 | `rtt` 在另一核有負載時的量測 |

> 這次的教訓：**「雙核獨立」不等於「同核也獨立」**。fx 測試任務（prio 3）與 uart.rx（prio 3）
> 同級，就把 UART 的 CRC 事件率由 1/3,150 推到 1/900。

---

## 5. 偶發事件（`crc_err`／`slot_full`）怎麼追

1. **先問「有沒有容量壓力」**：`slot_full`、`peak` 對比 slots；`max_dlen` 對比 region。
   沒有壓力就**不要**先加大緩衝（這次加大環完全沒用）。
2. **單變量對照**：一次只改一個（環大小、優先級、流量），每組跑 ≥ 10 分鐘。
3. **比事件率，不比有無**：`事件數 / 循環數`，並算一下「若率不變，全乾淨的機率」。
   （例：1/545 的率下 1,836 循環全乾淨只有 ~3% → 才能說「修好了」。）
4. **要有獨立證據證明「沒有掉幀」**：例如每輪 `rx` 都收齊；否則分不清
   「掉幀」與「多出雜訊」。
5. 追不到就**如實記錄率與已排除項**，把下一步的診斷寫清楚（這次是「數 RX FIFO overrun」）。

---

## 6. 陷阱清單（每一條都踩過）

| # | 陷阱 | 症狀 | 解法 |
|---|---|---|---|
| 1 | 量測工具自己的延遲 | RTT 全部 ≈55 ms | pyserial `timeout` 用 **1 ms**，不要 50 ms |
| 2 | 輪詢量化 | UART 時間永遠是 50 ms 的倍數 | 輪詢間隔 **5 ms**，或用裝置側計數器判完成 |
| 3 | 用 handler 灌流 | 只收到 62/200 幀、像掉幀 | handler 揸 decoder 鎖 → 用**獨立 task** 的指令（`0xF00E`） |
| 4 | 計數器累加 | before/after 數字沾到上一輪 | `session` 先送 `0xF008 reset=1`（測試映像） |
| 5 | 跨版本比 heap | 差額對不上（±數 KB） | 只在**同一次開機**內用 config 對照；跨版本看逐項 log／HWM |
| 6 | CMake cache | 以為建 release，其實是 tests | `make.py build … --tests`；側錄有 `"tests"` |
| 7 | 同核同級任務 | 偶發 CRC／溢位，`slot_full=0` | 租戶優先級 < producer |
| 8 | 只看 HWM | 沒跑過的路徑沒覆蓋 | `tools/stack_cover.py` 逐條打已註冊指令 |
| 9 | 只跑一次 | 以為變快／變慢 | 3 次取中位數；`compare` 的 3% 門檻 |
| 10 | 用 `sizeof(指標)` 當緩衝長度 | 回覆靜靜消失（`pack_into` 回 -1） | 改用具名常數（`NC4_REPLY_FRAME_MAX`） |

---

## 7. 模板：量測紀錄 ＋ 決策

貼進 PR／commit／`status.md`：

```markdown
### <日期> <主題>

**目標**：<一句話；要證明什麼>

| 項目 | 之前 | 之後 | 依據（哪個計數器／哪一輪） |
|---|---:|---:|---|
| … | … | … | … |

**負載**：<指令；fixture；每次幾幀／幾 bytes>
**A/B**：session before=<檔> after=<檔>；退步項 0（門檻 3%）
**偶發**：<率、已排除項、下一步>
**回退條件**：<什麼情況下改回去>
```

決策紀錄另外要有：

- **為甚麼是這個值**（峰值＋邊際的算式）
- **留了多少邊際**（例：棧 2,232 used → 3,072，剩 840）
- **回退條件**（例：`slot_full > 0` 或吞吐退步 > 3% 就回復上一組）

---

## 8. 安全與回退（S3 只有 OTA 一條路）

1. **config 先行**：`/Volumes/<NC-XXXX>/config/*.json` 改完 → `REBOOT`（換韌體才會重新格式化，
   只改 config 不會）。能不燒片就不燒片。
2. **燒片**：只用本機建置過、boot 路徑 diff 小的映像；`tools/ota.py` 會驗 SHA256，
   `--no-apply` 可以只寫不切換。沒有 rollback，映像壞了要人手（BOOT 鍵）。
3. **一次一組**：把「改尺寸」與「改行為」分開燒，出事才知道是哪一組。
4. **artifacts 側錄**：`git`／`idf`／`sha256`／`tests` 都記在 `.json`，回報時附上。
5. **需要人手的測試排最後**：換接線、按鍵、斷電、拔插——先自動化能做的。
