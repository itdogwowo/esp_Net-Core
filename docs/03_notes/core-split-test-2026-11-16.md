# 同 Interface 兩條 TCP port：decode0 vs decode0+1

日期：2026-11-16
Board：ESP32-S3-N16R8（WiFi STA AX3000，10.10.1.31）
目的：確認同一 interface 的兩條 TCP line，分開兩個 decoder core 是否真的更快。

## 方法

- 新增第二條 TCP line：`:6002`（`TCP2`）。
- 兩個 port 同時各跑 `net_bench.py --mode tcp --count 500 --burst`。
- 比較：
  - A：`TCP -> decode0`、`TCP2 -> decode0`
  - B：`TCP -> decode0`、`TCP2 -> decode1`
- 每個設定跑 3 次；量 aggregate replies/s。
- 設定經 WebMaster `/api/file` 寫入 `/config/system.json`，reboot 後測試。
- 測試後已移除 `TCP2`，恢復原設定。

## 結果

### A：兩條都在 decode0

| Run | TCP :6000 | TCP2 :6002 | Aggregate |
|---|---:|---:|---:|
| 1 | 387.0 | 196.2 | **583.2/s** |
| 2 | 195.6 | 390.0 | **585.6/s** |
| 3 | 376.5 | 184.5 | **561.0/s** |
| 平均 | — | — | **576.6/s** |

### B：TCP2 去 decode1

| Run | TCP :6000 | TCP2 :6002 | Aggregate |
|---|---:|---:|---:|
| 1 | 373.5 | 190.6 | **564.1/s** |
| 2 | 376.0 | 188.8 | **564.8/s** |
| 3 | 190.6 | 378.1 | **568.7/s** |
| 平均 | — | — | **565.9/s** |

## 對比

| 設定 | Aggregate 平均 |
|---|---:|
| 同一 decoder0 | **576.6/s** |
| 分 decode0 / decode1 | **565.9/s** |

- 分 core **沒有提升**，反而約 **-1.8%**（在量測噪聲內，但沒有正收益）。
- 兩條 port 同時跑，aggregate 約 576/s；單一 port 之前可到 ~950–1,000/s。
- 即係：**多一條 port 不是多一份 WiFi／lwIP 能力**；共享 interface 反而令總量下降。
- 兩條 line 各自有自己的 bus／parser；decoder 之間冇直接鎖，但 WiFi driver／lwIP／socket 仍然共享。

## 結論

- 同一 physical interface（`wifi_*` 或 `eth_*`）的所有 NC4 port，**預設全部 map 同一個 decode core**。
- 分 core 不會增加 interface 頻寬，亦沒有量到好處。
- 只有将来量到單一 decoder core CPU 飽和，而且 action 並發安全，才考慮分 core。
