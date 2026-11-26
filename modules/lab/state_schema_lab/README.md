# state_schema_lab — schema 字典狀態儲存原型（2026-11-26）

用家提出：舊專案 `slave/schema/` 用 schema 做資料；狀態 key 有限（硬件有上限），
所以儲存用「schema 定 path→u16 id」，機存只寫 (id, value)，唔存 key 字串。

## 點跑

```powershell
python modules/lab/state_schema_lab/schema_preview.py
```

## 結果（Matrix，25 個 schema key）

| 檔案 | JSON | NCS1 blob（id+值） |
|---|---:|---:|
| hardware.json | 5,992 B | — |
| driver.json | 2,830 B | — |
| 合計 | **8,822 B** | **85 B（省 99.0%）** |

85 B = 實際有改嘅 21 個 key；default 唔存（sparse）。全 config 估計 150–400 B。

## 設計

- key id：`domain(4b) | unit(4b) | index(4b) | field(4b)`，u16。
- 儲存：`"NCS1" | ver u16 | count u16 | crc32 u32` + `(id varint, value zigzag varint)`。
- 範圍／型別／預設全部喺 schema；超出即 parse error（唔截斷）。
- 同 NCB2 分工：NCS1＝固定 schema 最省；NCB2＝動態／自描述（pixel assets 等）。

詳細：`docs/03_notes/state-schema.md`。
