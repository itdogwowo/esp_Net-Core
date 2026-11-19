# 測試 ID：哪些模組可以進去、哪些不可以

> **狀態：契約已定，CMake 已接（2026-11-19）。** 具體搬 `test_action` 的分類是下一步。
> 相關：[test-build-levels.md](test-build-levels.md)（舊的分級草稿，已被本頁取代）、
> [commands.md §1.1.1](../01_protocol/commands.md)、[configuration.md §6](../02_guides/configuration.md)

---

## 1. 為什麼要 ID

「測試」不是一種東西：量測、網路、pixel、ncm 互相污染（實例：`espnow_boot` 令記憶體
多 56 KB、`0xF062` 跑第二次會重置）。所以每個模組要**聲明自己屬於哪一類**，
build 時只收允許的類別。

**ID 不是等級、不是累加**：`NC_TEST_IDS=0,3` 就是「公共 + pixel」，不含量測。
要累加就自己列出來。

## 2. ID 表（0 是「所有人都可以進去」）

| ID | 名稱 | 內容 | 誰會收 |
|---:|---|---|---|
| **0** | **public（公共）** | 身分、狀態、記憶體量測等**任何 build 都需要**的東西 | **所有 build 永遠收**（不用列） |
| 1 | measure | CPU／記憶體／緩衝 bench（`0xF001`、`0xF01C`、`0xF030`、`0xF050`…） | 有列 `1` 才收 |
| 2 | network | TCP／UDP／WS／HTTP 測試、WiFi／ESP-NOW、rawbench | 有列 `2` 才收 |
| 3 | pixel | `0xF060`–`0xF062`（producer／render／PXBENCH） | 有列 `3` 才收 |
| 4 | ncm | loader／WAMR／arena（外掛宿主測試） | 有列 `4` 才收 |
| 5–7 | 保留 | 將來的新分類 | 有列才收 |

規矩：

1. **0 永遠在**：`NC_TEST_IDS` 寫什麼都會自動加入 0；任何 build 都不會「完全沒有測試」。
2. **一個模組可以有多個 ID**（`"test_ids": [1, 3]`）：
   - 收錄規則 = **任何一個** ID 被允許就收（`0` 在清單裡 → 永遠收）。
   - 例：一個 bench 同時服務 measure 與 network → 宣告 `[1, 2]`，
     `NC_TEST_IDS=0,2` 與 `NC_TEST_IDS=0,1` 都會收它。
   - C 碼寫法：`#if NC_TEST_ALLOW_1 || NC_TEST_ALLOW_2`（每個都是 0/1）。
3. **但「一條指令」只可以登記一次**：`0xF0xx` 在指令表只可出現一次
   （否則 action 管理器會當重複丟棄）。共用就把 handler 寫成普通函數，
   兩邊各一行表項但用 `#if` 保證**同時只編一邊**；或者只放在最低的 ID。
4. **開關是外層**：`NC_ENABLE_TESTS=OFF` 時 ID 無意義（整個測試層不存在）。
5. **兩條路徑共用同一份 ID 清單**：
   - **native**（編進固件）：`build.json` 的 `"test_ids": [..]`，build 工具過濾目錄；
     C 碼用 `#if NC_TEST_ALLOW_N` 包住可選段落。
   - **module file**（`.nct/.nce/.nca`）：manifest 帶同一份 `test_ids`，ncm 只載入
     允許的 ID（runtime 也要守同一條規則）。

## 3. 用法

```bash
# 公共 + 網路 + pixel（不含 measure、ncm）
python make.py build s3 --tests --tests-ids=0,2,3
# 等價的直接傳：
idf.py -B artifacts/build/... -DNC_ENABLE_TESTS=ON -DNC_TEST_IDS=0,2,3 build
```

CMake 會為 0–7 每個 ID 產生 `NC_TEST_ALLOW_<id>=1/0`，C 這樣用：

```c
#if NC_TEST_ALLOW_2
    /* network 類指令／hook 才編進來 */
#endif
```

## 4. 驗收

| 檢查 | 方法 |
|---|---|
| 只有允許的 ID 進來 | `status cmds` 不含未列 ID 的指令（例：`NC_TEST_IDS=0` 時冇 `0xF060`） |
| 0 一定在 | 任何 build 的 `status cmds` 都有公共指令；log 印「測試 ID：0 …」 |
| 兩條路徑一致 | 同一個 `build.json` 的 `test_id`，native 與 module file 得到相同取捨 |
| 沒有隱藏開關 | `grep NC_TEST_ALLOW` 只出現在該 ID 的段落，不出現在 `main.c` 的清單 |

## 5. 現況與下一步

- 已做：`NC_TEST_IDS`（CACHE）、0 自動加入、`NC_TEST_ALLOW_0..7` 產生、
  `main`／`test_action` 都收到這些 define。
- 未做：`test_action` 的 58 條按 ID 分段（`#if NC_TEST_ALLOW_1/2/3/4`）；
  `modules/` 的 `build.json` 掃描與過濾；`make.py --tests-ids` 參數。
