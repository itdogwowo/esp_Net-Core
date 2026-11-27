# 動態 key：任務／效果（2026-11-27）

> 定案：**字典（schema/state-keys.json）只放「固定」的東西**。會熱插拔的任務與效果
> **唔入字典**，改由管理器提供身份；state 用「名的 hash」做 key。

## 1. 為什麼任務唔可以入字典

- 任務係模組（`.nct`）帶入來的：`/data/tasks/*.nct` 隨時加／減，開機掃到才知有幾多個。
- 如果字典用「index 0..19 × level／affinity／prio」靜態化：
  - 插一個新任務 → index 可能位移 → 舊值對錯任務（**比冇值更危險**）；
  - 拔一個任務 → 它的 key 永遠 missing，但字典仍然宣稱存在。
- 效果（`.nce`）同理：目前只有幾條固定曲線，但設計上要當「隨時會多」。
- **結論**：任務／效果不進字典；由管理器做唯一權威，state 只幫它存值。

## 2. 身份：用名 hash，唔用 index

```text
key = 0xF0 << 24 ｜ (FNV-1a(name) & 0xFFFFFF)
例： "render" → 0xF0xxxxxx（同一個名永遠同一 key）
```

- **名**係穩定身份：改名＝新身份（預期）；插新模組、改載入次序都唔會令舊值對錯人。
- 24-bit（1/16M）碰撞：管理器載入時用 `nc_state_key_is_dyn()`／重算自查，
  撞到要**大聲 log**（唔好靜默蓋值）。
- 為什麼唔直接用任務管理器自己的 pointer／id：pointer 每次開機都變，
  id 由管理器遞增同樣會因熱插拔而漂移。名 hash 係唯一跨開機穩定的東西。

## 3. 落地要點（已做 vs 未做）

| 項目 | 狀態 |
|---|---|
| `nc_state_name_hash()`（FNV-1a 32） | ✅ 已做（host test 有覆蓋） |
| `nc_state_key_is_dyn()`／`nc_state_dyn_set_s32()`／`nc_state_dyn_get_s32()` | ✅ 已做（唔查 schema，呼叫者驗範圍） |
| `nc_state_store_prune()` 保留 0xF0 域 | ✅ 已做（否則 boot 一 prune 就掃走任務值） |
| 任務管理器用 name-hash 讀寫（level／affinity／prio） | ⬜ 下一步（要同 `nc_task_list_json()` 對齊） |
| 效果管理器掃 `.nce` 後建 id→name 表 | ⬜ 下一步（「掃完再給 key」＝掃到幾個、逐個配 hash） |
| 碰撞自查＋log | ⬜ 隨管理員一齊做 |

## 4. 效果器的「掃描再給 key」

用家要求：**掃描有幾多個效果，然後再給它們 key**。落地方式：

1. 開機掃 `/data/pixel/effects/*.nce`（同 `.nct` 同一套 loader）；
2. 建立 `效果清單`（檔名／manifest 內 id＝穩定名）；
3. 逐個 key = `0xF0` 域 + hash(id)；**數量只用來配置容量／報告**，
   唔用 index 做 key（加一個效果唔會令其他效果的值走位）。

## 5. 同固定 key 的分工（不變量）

- 固定硬體（PIN／I80／SPI／LED／links／routes／wifi／sys）：字典靜態、可驗範圍、可 activation。
- 任務／效果：字典**冇**記錄；key 由管理器提供；`set` 唔查 schema；
  prune 唔掃；回寫與固定 key 用同一個 blob（`src_hash` 只計 cfg 值集）。
- 所以：**拔走一個任務，它的 hash 值仍然留在 blob 裡**（孤兒），下次管理器掃到同名
  任務會重用；長期孤兒由「管理器通報清單」＋commit 時 prune 處理（下一步）。
