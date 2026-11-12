# CDC FAT：NC4 檔案指令（C 版）

> 2026-11-12。**為什麼要有**：部分 MCU 冇 USB driver，唔會枚舉成隨身碟（MSC），
> 唯一通道就係 USB CDC。所以檔案一定要可以經 NC4 直接操作，唔可以倚賴 MSC。
>
> 程式：`firmware/components/action/file_action/nc_file_action.c`
> UI：`tools/esp_webmaster/`（檔案頁）
> 指令表：[../01_protocol/commands.md](../01_protocol/commands.md)

---

## 1. 指令總表

| CMD | 名稱 | 方向 | Payload |
|---|---|---|---|
| `0x2001` | FILE_BEGIN | host → dev | `file_id(u16) total(u32) chunk(u16) sha256(32B) path(str)` |
| `0x2002` | FILE_CHUNK | host → dev | `file_id(u16) offset(u32) data(rest)` |
| `0x2003` | FILE_END | host → dev | `file_id(u16)` |
| `0x2004` | FILE_ACK | dev → host | `file_id(u16) offset(u32)`（BEGIN 回 offset=0；CHUNK 回該包 offset） |
| `0x2005` | FILE_QUERY | host → dev | `path(str)` |
| `0x2006` | FILE_QUERY_RSP | dev → host | `exists(u8) sha256(32) size(u32) path(str) free(u32) pending(u8)` |
| `0x2007` | FILE_READ | host → dev | `path(str) offset(u32) length(u16)` |
| `0x2002` | （讀取回覆） | dev → host | `file_id=0(u16) offset(u32) data(rest)` |
| `0x2008` | FILE_CONFIRM | host → dev | `path(str)` → `0x2006` |
| `0x2009` | FILE_DELETE | host → dev | `path(str)` → `0x2006` |
| `0x200A` | FILE_UNDO | host → dev | `path(str)` → `0x2006` |
| `0x200B` | FILE_SCAN | host → dev | `target(u8)`（0=local／1=SD）；**冇回覆**，掃完寫 `/manifest.json` |
| `0x2010` | FILE_ERROR_RSP | dev → host | 7×bool + `failed_offset(u32)` + `written_up_to(u32)` + `path(str)` |

> **冇 directory listing 指令**（schema 本身都冇）。跟 mp 版：`FILE_SCAN`
> walk `/data`，寫一份 `/manifest.json`，UI 用 `FILE_READ` 讀一次：

```json
{"/config/system.json":{"s":112,"h":"<sha256 hex>","p":0},
 "/pixel/effects/demo.json":{"s":84,"h":"...","p":1}}
```

`s` 檔案大小、`h` sha256 hex、`p=1` 有 `.bak`（待確認）；
`.bak` / `.part` / `.esptmp` / `manifest.json` 自己唔會列入。
**冇目錄 entry**：UI 由 path 的 `/` 自行砌樹。

---

## 2. 寫入流程（逐步）

```text
FILE_BEGIN  →  開 <path>.part，PSA hash 開始           → FILE_ACK(offset=0)
FILE_CHUNK  →  offset == written？寫入＋hash_update   → FILE_ACK(offset)
   ⋮
FILE_END    →  close → hash_finish → 對 sha256
               ├─ 不符 → 刪 .part，回 0x2010 err_sha_mismatch
               └─ 符合 → 舊檔 rename .bak → .part rename 正式
                        → 回 0x2006（pending = .bak 存在）
```

規則：

- 一次只准一個 session（`s_active`）；第二個 BEGIN 回 `err_busy`。
- **閒置 30 秒**（上一條連線斷咗、冇 END）會自動 abort 並刪 `.part`，
  唔會令裝置永遠 busy。
- `chunk_size` 1–4096（CDC rx region 4,192；幀 = chunk + 17）。
- `offset` 必須等於已寫入量；否則 `err_offset_mismatch`（可重試）。
- 寫入需要 `nc_store_app_ready()`（裝置擁有 FAT）。
  USB 模式（host 掛住碟）寫入一律 `err_busy`；讀取仍然可以走 `/ro` 唯讀視圖。
- `total` 大於 free 回 `err_no_space`。
- 所有操作經一把 mutex（兩個 core 的 decoder 可能同時打）。

### pending / confirm / undo

`pending` **唔係**存在 NVS 的旗標，而是「`.bak` 存在」的推導：

| 動作 | 效果 |
|---|---|
| `FILE_CONFIRM` | 刪 `<path>.bak` → pending=0 |
| `FILE_UNDO` | 刪 `<path>`，`.bak` rename 回 `<path>` |
| `FILE_DELETE` | 刪 `<path>`、`.bak`、`.part` |

首次寫入新檔冇 `.bak`，所以 pending=0；覆蓋舊檔才有 pending。

> **UI／server 默認自動 confirm**：`FILE_END` 成功後即刻送 `0x2008`，
> 所以正常上傳唔會停喺「待確認」。只有 confirm 失敗才保留 pending，
> 讓 UI 重試。協議層仍然保留 `0x2008/0x200A`；`0x200A UNDO` 主要用於
> raw client／救援，正常 UI 流程唔會見到。

> **manifest 只由 `0x200B FILE_SCAN` 寫。** `FILE_END` / `DELETE` / `CONFIRM` /
> `UNDO` **唔會**即時改 manifest —— 實測喺 decode 4 KB 棧上跑 manifest 的
> cJSON/FAT 會 panic（`rst=4`）。UI 每次改檔後自動送一次 `FILE_SCAN` 再讀
> manifest；872 KB FAT、11 個檔實測約 0.8 s。

---

## 3. 讀取 / manifest

- `FILE_READ` 每次最多 4,096 B；回覆用 `0x2002`（`file_id=0`）。
  UI 先 `FILE_QUERY` 拿 size，再逐段讀到 size 為止。
- **檔案清單 = `FILE_SCAN` + manifest**：
  - UI 送 `0x200B`（`target=0`），之後 poll `FILE_QUERY /manifest.json`；
  - 有 manifest 就 `FILE_READ /manifest.json` 一次，前端用 path 砌樹；
  - 唔使逐層問，N 個目錄都只係一次讀取（mp 版同款）。
- manifest 上限 200 個檔／32 KB；scan 會跳過 `.bak` / `.part` / `.esptmp`。
- `/sd` 前綴直接映射到 SD 掛載點；v1 只讀（寫入回 `err_busy`），
  `FILE_SCAN target=1` 暫未支援。

---

## 4. 錯誤碼

`0x2010` 的七個 bool（順序固定）：

`err_no_space`、`err_write_fail`、`err_offset_mismatch`、`err_id_mismatch`、
`err_sha_mismatch`、`err_not_active`、`err_busy`

UI 對 `err_busy` 的提示：先按「接管 FAT」（`0x1103 file_mode=1`）再試。

---

## 5. UI 對接（STATUS 欄位）

真機 `STATUS_GET` 回傳的欄位同 `--sim` 對齊：

| 區塊 | 欄位 | 用途 |
|---|---|---|
| `sys` | `chip`、`fw`、`up_s`、`heap` | 裝置摘要 |
| `storage` | `mode`（app/usb）、`free`、`total`（bytes）、`app_ready` | FAT 狀態／容量 meter |
| `log` | `produced`、`sent`、`dropped`（可能是 per-core 陣列） | 終端統計 |

`storage.free/total` 由 `f_getfree()` + `(n_fatent-2) × cluster size` 算出，
單位 bytes（同 UI 的 `fmtBytes` 一致）。`storage.pending` 未提供；
UI 由 manifest 每個 entry 的 `p` 旗標顯示「待確認」。

---

## 6. 未做（v1 範圍外）

- 斷點續傳（`0x200E/0x200F`）
- `FILE_MOVE`（`0x200D`）、`FILE_PROMOTE`（`0x2011`）
- SD 寫入
- 資料夾建立／刪除（UI 只操作檔案）
- `0x1019 ASSET_RELOAD`：拖完檔仍然要 `0x100F REBOOT` 才生效

---

## 6. 測試

- 協議／UI：`cd tools/esp_webmaster && python -m pytest -q`
  （sim 實作同一套指令，共 37 個測試，含 firmware↔UI 指令碼契約、C 語法 parse、OTA chip guard）
- C 檔語法／型別：用 MSVC `cl /Zs` ＋ stub headers 檢查通過（唔係 IDF build）。
- **真 IDF build（2026-11-12，v6.1，WSL）**：
  - `ESP32-S3-N16R8 --tests` ✅ 646.5 KB（分區剩 84%）
  - `ESP32-S3-N16R8` ✅ 511.3 KB（88%）
  - `ESP32-S3-Matrix` ✅ 491.3 KB（4MB、無 PSRAM）
  - `ESP32-P4-ETH` ✅ 1006.8 KB（75%）
  - `ESP32-P4-C6` ✅ 973.8 KB（76%）
- 真機：`python run.py --serial COMx` → 檔案頁列目錄、上傳、下載、編輯、刪除。
- 無 IDF 環境：C 檔未 compile；要喺有 IDF 的機跑
  `python make.py build s3` / `python make.py build p4`。
