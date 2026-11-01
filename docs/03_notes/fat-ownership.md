# FAT 的擁有者（讀寫分離模型）

> **狀態**：已實作並在 S3 上實測（2026-11-02）
> **程式**：`firmware/components/sys/store/`（仲裁 + 唯讀視圖）、`firmware/components/app/file_cmds/`（指令）
> **相關**：[commands.md](../01_protocol/commands.md)（指令表）、[component-layout.md](component-layout.md) §8（資產）

---

## 1. 問題

`data` 分區是一個 FAT。同一時間只能有一個主人：

| 主人 | 主機看到 | 裝置看到 |
|---|---|---|
| USB host（MSC） | 隨身碟 `/Volumes/NETCORE` | 只能透過唯讀視圖 |
| 裝置（APP） | 碟消失 | `/data` 可讀可寫 |

**決定：讀寫分離。**

- **讀**：任何時候都答得到（主機用碟的同時，裝置掛一份唯讀視圖）
- **寫**：才需要接管（主機的碟暫時失效），寫完還原

---

## 2. 為什麼不能只切 `mount_point`

`tinyusb_msc_set_storage_mount_point(APP)` 只換「FatFS 掛在哪」：

- MSC 的 `READ10` / `WRITE10` 回呼**不看** `mount_point`，只看 `host_io_blocked`
  （那個旗標只在切換「進行中」為真）。
- 只有 `TEST UNIT READY` 會回 not-ready，而主機 mount 之後就不再問了。

實測（S3，切到 APP 之後）：主機寫 64 KiB 仍然真的落到 flash，花 **7.3 秒** ——
也就是裝置與主機同時在寫同一塊 FAT，會壞檔。

---

## 3. 現在的做法

```text
        ┌──────────── 主機（USB）────────────┐
        │ 擁有 FAT，可自由讀寫               │
        └──────────────┬─────────────────────┘
                       │ 同一塊 WL
        ┌──────────────▼─────────────────────┐
        │ 裝置：唯讀視圖 /ro（pdrv 1）        │  ← 讀：永遠可用（可能過期/半截）
        └──────────────┬─────────────────────┘
                       │ 要寫入時才接管
        ┌──────────────▼─────────────────────┐
        │ 裝置：/data 可讀寫（pdrv 0，APP）   │  ← 寫：主機的碟暫時消失
        └────────────────────────────────────┘
```

| 模式 | 裝置的掛載 | 主機 | 誰能寫 |
|---|---|---|---|
| **USB（預設）** | `/ro`（唯讀，pdrv 1） | 看到碟 | 只有主機 |
| **APP（接管）** | `/data`（pdrv 0） | 碟失效 | 只有裝置 |

切換方式（用既有指令，**沒有新增指令號**）：

```jsonc
// 0x1103 STATUS_SET { data_json, persist }
{"System":{"file_mode":1}}   // 接管（寫入前）
{"System":{"file_mode":0}}   // 交還（寫完）
// 回覆 0x1104 STATUS_ACK { ok, message }
```

`file_mode` 不落盤（`persist` 忽略）；它是執行期狀態。

### 為什麼唯讀視圖是安全的

- `wear_levelling` 每個 handle 有自己的 lock → 區塊層不會交錯損壞。
- 唯讀視圖只做讀，不寫 → 不可能破壞主機的 FAT。
- 代價：主機正在寫入時，讀到的內容可能過期或半截 → 回覆的 `pending=1` 就是用來標記這件事。

---

## 4. 實測數據（S3，2026-11-02）

| 步驟 | 結果 |
|---|---|
| USB 模式 `STATUS_GET storage` | `{"storage":{"mode":"usb","app_ready":0}}` |
| USB 模式 `FILE_QUERY /config.json` | `exists=1 size=415 sha=712b48b1… free=211 KiB pending=1` |
| `STATUS_SET {"System":{"file_mode":1}}` | `OK file_mode=1 mode=app` |
| claim 後主機寫 64 KiB | **失敗，0.19 s**（修正前是 7.3 秒真寫入） |
| APP 模式 `FILE_QUERY` | 同上但 `pending=0`（真值） |
| `STATUS_SET {"System":{"file_mode":0}}` | 約 1 秒後 USB 重新列舉，碟幾秒內回來 |
| sha256 交叉驗證 | 裝置回報 = 主機 `sha256sum`，完全一致 |

媒體速度（同一塊板）：

| 路徑 | 實測 |
|---|---|
| MSC 讀 | 0.80 MB/s |
| MSC 寫 | 0.075 MB/s |
| 裝置直寫 4K（raw） | 709 µs/op（5.8 MB/s） |
| 裝置擦除 4K（raw） | **38.6 ms/op** ← 真正的瓶頸（上限約 106 KB/s） |

---

## 5. 寫入 session（還沒實作，這是約定）

```text
0x2001 FILE_BEGIN ──► file_mode 自動切到 APP（收 FAT）
0x2002 FILE_CHUNK ×N
0x2003 FILE_END   ──► 驗 sha → 交還 FAT
   閒置 N 秒沒有 CHUNK ──► abort + 交還 + 回 0x2010
   任何時候 0x1103 {"System":{"file_mode":0}} ──► 強制交還（取消）
```

規則：**接管時間越短越好**；主機在 APP 模式看到的碟是失效的。

---

## 6. 還沒做的

- `0x2001/0x2002/0x2003`（FILE_BEGIN/CHUNK/END）、`0x2007`（FILE_READ）、
  `0x2009`（DELETE）、`0x200D`（MOVE）—— 讀取骨架已就位，寫入路徑還沒接。
- `pending` 目前只當「唯讀視圖」旗標用；真正的 `.bak`/delta 機制未做。
- `0x1103` 目前只實作 `System.file_mode` 一個 key，放在 `file_cmds` 裡；
  等 config tree 落地要搬到獨立的 status 元件。
