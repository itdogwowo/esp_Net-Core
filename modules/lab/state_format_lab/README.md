# state_format_lab — 狀態儲存格式原型（2026-11-26）

一次性 lab：驗證「唔直接存 JSON、改存緊湊二進位」嘅壓縮率，定 NCB2 header 格式。

## 點跑

```powershell
python modules/lab/state_format_lab/ncb2_preview.py
```

## 結果（2026-11-26，Matrix 預置）

| 檔案 | JSON B | NCB2 B | 省 |
|---|---:|---:|---:|
| hardware.json | 5,992 | 3,462 | 42.2% |
| driver.json | 2,830 | 1,618 | 42.8% |
| system.json | 2,274 | 1,378 | 39.4% |
| tasks.json | 1,494 | 1,134 | 24.1% |
| resources.json | 519 | 483 | 6.9% |
| 7 個檔合計 | 13,971 | 8,788 | **37.1%** |

呢個係通用 encoder（連 key 名）；固定 schema（field id、唔存 key）預計再細 50%+。

## 格式（NCB2 v2）

header 20 B：`"NCB2" | ver u16 | flags u16 | strtab_len u32 | payload_len u32 | crc32 u32`
＋ key 字典（每個 key 只存一次）＋ TLV 值樹（zigzag varint；bool 1 B）。
詳細同落地計劃：`docs/03_notes/state-storage-format.md`。
