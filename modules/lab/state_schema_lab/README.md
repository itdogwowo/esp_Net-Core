# state_schema_lab — schema／硬件矩陣原型（2026-11-26）

兩個原型：

| 腳本 | 做咩 | 結果 |
|---|---|---|
| `schema_preview.py` | JSON 路徑 → (id, 值) 的 NCS1 blob | hardware+driver 8,822 B → **85 B（99.0%）** |
| `hw_matrix_preview.py` | 硬件矩陣＋儲存上限＋模板 JSON＋GPIO bitmap | **356 keys**；worst **2,504 B**；typical 236 B；模板 5,082 B |

## 點跑

```powershell
python modules/lab/state_schema_lab/schema_preview.py
python modules/lab/state_schema_lab/hw_matrix_preview.py
```

設計文件：`docs/03_notes/state-schema.md`（NCS1）＋`docs/03_notes/hw-matrix.md`（矩陣／上限）。
