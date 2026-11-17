# NCM 格式標準（`.nct` / `.nce`）

> **用途**：這是 `.nct` / `.nce` 的**格式標準**——位元組佈局、magic、驗證規則、錯誤碼。
> 實作在 `firmware/components/tasks/test_mod_tasks/`（`nc_mod.c` + `nc_ncd.c`），
> 打包／檢視在 `tools/ncm.py`。
> **狀態**：v1（2026-11-17）。容器、manifest、指令表、wasm/aot 兩種 payload 都已實作。
> **相關**：[ncm-loader.md](ncm-loader.md)（載入流程）、[ncm-engine.md](ncm-engine.md)（引擎）、
> [ncm-contract.md](ncm-contract.md)（ABI 與記憶體邊界）

---

## 1. 兩種副檔名

| 副檔名 | 意思 | 預設 kind | 給誰看 |
|---|---|---|---|
| `.nct` | **Net-Core Task** | `task`（1） | 人（檔案總管、拖放） |
| `.nce` | **Net-Core Effect** | `effect`（2） | 人（檔案總管、拖放） |

**副檔名只是人看的分類標籤。** 真正的 kind 以 manifest 為準：

- 有 manifest → 用 manifest 的 `kind`；與副檔名不同時記 warning，**manifest 為準**。
- 沒有 manifest → 用副檔名當預設（`.nct` = task、`.nce` = effect）。

---

## 2. 容器佈局

全部 little-endian。

```
+0    48 B   header      怎麼載入（magic / abi / flags / 大小 / crc / target / manifest 位置）
+48   40 B   manifest    怎麼活在系統裡（kind / core / layer / prio / fps / px / stack / budget / mem / cmds）
+88   N×2 B  cmd table   它擁有哪幾條指令（u16，可選，4-byte 對齊）
+..   payload            wasm 或 aot（真正的程式碼）
```

`payload_off` 由打包工具決定（通常是 `align(88 + ncmds*2, 4)`）；載入器只信檔頭。

---

## 3. Header（48 B）

| off | size | 欄位 | 說明 |
|---|---|---|---|
| 0 | 4 | `magic` | `'N','C','M','1'` = `0x314D434E` |
| 4 | 2 | `abi` | 目前 `0`；不符就拒絕 |
| 6 | 2 | `flags` | bit0 = 有 AOT；bit1 = 有 manifest |
| 8 | 4 | `total_size` | 整個檔案；用來擋截斷 |
| 12 | 4 | `payload_off` | payload 的位移（通常 88 或 4-byte 對齊後） |
| 16 | 4 | `payload_size` | wasm 或 aot 的長度 |
| 20 | 4 | `payload_crc` | CRC-32/ISO-HDLC（init 0，等於 `zlib.crc32`） |
| 24 | 4 | `target_id` | `0` = wasm（通用）、`1` = rv32-ilp32f（P4）、`2` = xtensa（S3） |
| 28 | 4 | `mem_bytes` | 這個模組要多少線性記憶體 |
| 32 | 4 | `px` | 它假設的像素數（0 = 非效果） |
| 36 | 4 | `fps` | 建議幀率（0 = 不排程） |
| 40 | 4 | `manifest_off` | manifest 的位移（0 = 沒有） |
| 44 | 4 | `manifest_size` | manifest 的長度（有 manifest 時 ≥ 40） |

```c
_Static_assert(sizeof(nc_ncm_hdr_t) == 48, "ncm header must stay 48 B");
```

**為什麼 AOT 是可選的**：第一版只出 `.wasm`；`flags` bit0 留著，等某個模組實測 > 8 ms/幀，
再為它單獨加 `.aot`——格式不用改。

**為什麼要 `target_id`**：同一份 `.wasm` 可以在 P4 與 S3 跑（interp），但 `.aot` 綁架構。
檔頭寫清楚，載入器才不會把 riscv32 的機器碼載進 S3。

**為什麼要 `mem_bytes`**：載入前就要知道要跟 arena 要多少，不能「先載入再說」。

---

## 4. Manifest（40 B）

| off | size | 欄位 | 說明 |
|---|---|---|---|
| 0 | 4 | `magic` | `'N','C','D','1'` = `0x3144434E` |
| 4 | 2 | `version` | 目前 `1`；不符就拒絕 |
| 6 | 1 | `kind` | `1` = task、`2` = effect；`0`（system）一律拒絕 |
| 7 | 1 | `core` | `-1` = 不限；`0` / `1` |
| 8 | 1 | `layer` | `0–7` |
| 9 | 1 | `prio` | `1–24`（0 = idle） |
| 10 | 2 | `fps` | `0–240`；0 = 不排程 |
| 12 | 2 | `px` | `0–4096`；0 = 非效果 |
| 14 | 2 | `flags` | 保留（目前 0） |
| 16 | 4 | `stack` | `0`（預設 4096）或 `1024–32768` |
| 20 | 4 | `budget_us` | 每幀預算，`0–100000` |
| 24 | 4 | `mem_bytes` | `0`（用檔頭）或 `0–131072` |
| 28 | 4 | `cmds_off` | 指令表在檔案裡的位移（0 = 沒有） |
| 32 | 2 | `ncmds` | 指令數，`0–32` |
| 34 | 2 | `reserved` | 0 |
| 36 | 4 | `reserved` | 0 |

```c
_Static_assert(NC_NCD_HDR_SIZE == 40, "ncd manifest must stay 40 B");
```

### 4.1 一份真相

manifest 的 `px` / `mem_bytes` / `fps` 若有寫，**必須等於檔頭**。兩處不一致 = 工具鏈壞了，
寧可拒絕（`ESP_ERR_INVALID_ARG`）。

### 4.2 沒有 manifest 也可以

沒有 manifest 的 `.nct` / `.nce` 仍然可以載入：

- `.nct` → kind = task
- `.nce` → kind = effect
- `core = -1`、`layer = 3`、`prio = 3`、`stack = 4096`
- `fps` / `px` / `mem_bytes` 取檔頭
- `budget_us` 用 ABI 預設（`NC_MOD_FRAME_BUDGET_US`）

**manifest 是加值，不是必要條件。**

### 4.3 Meta 區塊（可選）

manifest 的 `meta_size`（off 34, u16）與 `meta_off`（off 36, u32）指向一個 **540 B** 的 meta 區塊。
`meta_off = 0` 就是沒有 meta（舊行為）。

| off | size | 欄位 |
|---|---|---|
| 0 | 24 | `name`（UTF-8，NUL 補齊） |
| 24 | 64 | `desc` |
| 88 | 64 | `usage` |
| 152 | 1 | `n_requires` |
| 153 | 1 | `n_provides` |
| 154 | 2 | reserved |
| 156 | 8 × 24 | `requires[]` |
| 348 | 8 × 24 | `provides[]` |

每個 entry（24 B）：

| off | size | 欄位 |
|---|---|---|
| 0 | 1 | `type`（0=buf 1=cmd 2=hw 3=provide 4=module） |
| 1 | 1 | `flags`（bit0 = exclusive） |
| 2 | 2 | `bytes` |
| 4 | 20 | `name`（UTF-8，NUL 補齊） |

`ncm.py pack --meta meta.json` 會把 JSON 轉成這個二進位區塊；`dump` 會印出來。
`requires` / `provides` 會在開機時合併進 **外部任務資源表格**（`resources.json`），見
[config-layers.md](config-layers.md) 的 `resources` 層。

---

## 5. 指令表

`ncmds > 0` 時，`cmds_off` 指向一個 `u16[ncmds]` 陣列（little-endian）。

- 必須落在檔案內（`cmds_off + ncmds*2 ≤ total_size`）。
- 同一模組內不得重複。
- 不得已被別人註冊（指令有主人，見 [writing-a-task.md](writing-a-task.md) §2.2）。

---

## 6. Payload

| 格式 | `flags` bit0 | `target_id` | 誰產生 | 特性 |
|---|---|---|---|---|
| **wasm**（預設） | 0 | `0` | tinygo | 任何晶片都吃；直譯 |
| **aot** | 1 | `1` = rv32f（P4）、`2` = xtensa（S3） | `wamrc`（PC 上） | 快、大、**綁架構** |

載入時的四條檢查（全部在進 WAMR 之前）：

1. `flags` 說 AOT 但 `target_id = 0` → 拒絕（`ESP_ERR_INVALID_ARG`）
2. `flags` 沒說 AOT 但 `target_id ≠ 0` → 拒絕（自相矛盾）
3. AOT 的 `target_id` ≠ 這顆晶片 → 拒絕並講清楚（`ESP_ERR_NOT_SUPPORTED`）
4. 晶片架構未知（`nc_mod_arch_target() == 0`）→ 只收 wasm

---

## 7. 驗證順序與錯誤碼

載入器（`nc_mod_load_ex`）與掃描器（`nc_mod_probe`）共用同一組檢查：

| # | 檢查 | 錯誤碼 |
|---|---|---|
| 1 | 檔頭 magic = `NCM1` | `ESP_ERR_INVALID_ARG` |
| 2 | `abi == 0` | `ESP_ERR_NOT_SUPPORTED` |
| 3 | `payload_off ≥ 48`、`payload_size > 0`、`payload_off + payload_size ≤ total_size` | `ESP_ERR_INVALID_SIZE` |
| 4 | AOT / wasm 與 `target_id` 一致 | `ESP_ERR_INVALID_ARG` / `ESP_ERR_NOT_SUPPORTED` |
| 5 | `mem_bytes ≤ 128 KB`、`px ≤ 4096` | `ESP_ERR_INVALID_SIZE` |
| 6 | manifest magic = `NCD1`、version = 1 | `ESP_ERR_INVALID_ARG` |
| 7 | manifest `kind` ∈ {1, 2} | `ESP_ERR_INVALID_ARG` |
| 8 | manifest `core` ∈ {-1, 0, 1}、`layer ≤ 7`、`1 ≤ prio ≤ 24` | `ESP_ERR_INVALID_ARG` |
| 9 | `stack` ∈ {0} ∪ [1024, 32768]、`fps ≤ 240`、`px ≤ 4096`、`mem ≤ 128 KB`、`budget ≤ 100 ms` | `ESP_ERR_INVALID_ARG` |
| 10 | 指令表落在檔案內、不重複 | `ESP_ERR_INVALID_ARG` |
| 11 | manifest 與檔頭的 `px` / `mem` / `fps` 一致 | `ESP_ERR_INVALID_ARG` |
| 12 | payload CRC 相符 | `ESP_ERR_INVALID_CRC` |
| 13 | `mem_bytes ≤ arena` | `ESP_ERR_NO_MEM` |

**掃描階段（`nc_mod_probe`）只做 1–11，不讀 payload、不碰 arena。**
**載入階段（`nc_mod_load_ex`）做 1–13。**

---

## 8. 範例

```bash
# 打包一個效果
python3 tools/ncm.py pack --payload fire.wasm --out fire.nce \
    --kind effect --core 1 --layer 3 --prio 3 --fps 30 --px 676 \
    --mem 65536 --stack 4096 --budget-us 8000 --cmd 0x3101

# 打包一個任務
python3 tools/ncm.py pack --payload worker.wasm --out worker.nct \
    --kind task --core 1 --layer 4 --prio 3

# 檢視
python3 tools/ncm.py dump fire.nce
```

`dump` 輸出：

```
header  : magic=0x314D434E abi=0 flags=0x0002
          total=152 payload_off=88 payload_size=64 crc=2880FB99
          target=0 mem=65536 px=64 fps=30
          manifest_off=48 manifest_size=40
  manifest magic=0x3144434E ver=1 flags=0x0000 rsvd=0/0
manifest: kind=effect(2) core=-1 layer=3 prio=3 fps=30 px=64
          stack=4096 budget_us=8000 mem=65536 ncmds=0
payload : 64 B crc=2880FB99 → OK
```

---

## 9. 版本

| 版本 | 日期 | 變更 |
|---|---|---|
| v1 | 2026-11-17 | 由 `.fxe` 改名為 `.nct` / `.nce`；magic `NCM1` / `NCD1`；kind `1=task 2=effect` |

**改版規則**：

- 改 header 佈局 → `abi` +1，舊檔一律拒絕（不是靜靜跑錯）。
- 改 manifest 佈局 → `version` +1。
- 加欄位 → 用 `reserved` 或升 `version`；不要改既有欄位的位移。
- 改 magic → 等於新格式，舊檔全斷（這次就是）。
