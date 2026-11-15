# 網絡效能測試計畫（可重複執行）

> 目的：建立一份**完整的測試清單＋方法＋結果格式**，讓未來不用重新猜「這個數字是哪一種測試」。
> 這份是 plan／runbook；實際數據在 `net-results-2026-11-15.json`，
> 敘述與限制在 `net-test-matrix-2026-11-15.md`。

## 1. 每次測試必須記錄的 metadata

| 欄位 | 例子 | 為什麼 |
|---|---|---|
| `test_id` | `NC4-P4-8K-TCP` | 唯一識別 |
| `type` | `request_reply` / `oneway_nc4` / `raw_tcp` | 三種速度不可互比 |
| `board` / `ip` | P4 / 10.10.1.50 | 硬件／網絡 |
| `firmware` | shipment / `--tests` / rawbench | 決定有沒有 0xF040／raw endpoint |
| `driver` | old / new line | 新舊實作 |
| `direction` | pc_to_device / device_to_pc | raw 測試方向 |
| `payload` / `batch` / `send_cap` | 8000 / 10 / 4096 | 直接影響吞吐 |
| `buffer` | slots × slot_size / mem | SRAM／PSRAM |
| `core` / `prio` | core1 / 12 | 排程 |
| `result` | MB/s、RTT、replies/s | 量到的值 |
| `valid` | true / false | 失敗或條件不符不可當數據 |
| `notes` | timeout / RST / noise | 為什麼無效 |

## 2. 測試清單

### A. Request/reply

| ID | 工具 | 條件 | 指標 |
|---|---|---|---|
| RR-P4-TCP | `tools/net_bench.py --mode tcp` | shipment，common profile | RTT、replies/s |
| RR-P4-UDP | 同上 `udp` | 同上 | RTT、replies/s |
| RR-P4-WS | 同上 `ws` | 同上 | RTT、replies/s |
| RR-S3-* | 同上 | S3 STA | 同上 |

命令：

```sh
python3 tools/net_bench.py --ip 10.10.1.50 --mode tcp --count 100
python3 tools/net_bench.py --ip 10.10.1.50 --mode tcp --count 1000 --burst
```

### B. 單向 NC4（0xF040）

| ID | 工具 | 條件 |
|---|---|---|
| NC4-P4-{1,2,4,8}K-{TCP,WS} | `tools/netbench_one.py` | `--tests` firmware、背景任務停用 |
| NC4-S3-* | 同上 | 需要 S3 test build 的 0xF040（目前未成功） |

命令：

```sh
python3 tools/netbench_one.py --ip 10.10.1.50 --mode tcp \
  --count 500 --payload 8000 --batch 10 --timeout 10
```

### C. Raw TCP

| ID | 工具 | 條件 |
|---|---|---|
| RAW-P4-SINGLE | `tools/raw_tcp_bench.py` | `network.json rawbench=1`；source SRAM／4K cap |
| RAW-P4-DUAL | `tools/raw_tcp_dual.py` | 兩條 stream、sink/src 各 core |
| RAW-S3-* | 同上 | S3 WiFi |

命令：

```sh
python3 tools/raw_tcp_bench.py --ip 10.10.1.50 --total 4194304
python3 tools/raw_tcp_dual.py --ip 10.10.1.50 --total 8388608 --chunk 4096
```

### D. Buffer／memory

| ID | 變數 | 指標 |
|---|---|---|
| BUF-MEM | `mem=sram/psram/auto` | 吞吐、heap |
| BUF-SLOTS | TCP 1/2/4/8；WS 1/2/4 | burst、lost |
| BUF-SIZE | 8205／8192／16384／24576 | 大 frame／batch |

### E. 歷史對照（不可與上面直接混比）

| ID | 來源 | 說明 |
|---|---|---|
| HIST-MPY-P4-RAW | `mp_Net-Core/doc/01_protocol/08_performance_benchmark.md` | MicroPython raw TCP 9.35 @4K |
| HIST-MPY-P4-NC4 | 同上 | NC4 7.37 @16K |
| HIST-MPY-S3 | 同上 | WiFi ~0.8 |

## 3. 有效性規則

- `oneway_nc4` 必須 `frames == count`、`lost == 0`；否則標 `valid=false`。
- `raw_tcp` 必須 `got == total`；RST／broken pipe 標無效。
- request/reply 只可報 RTT／replies/s，不可換算成 MB/s。
- 同一條件至少跑 2 次；只取穩定的一次，不取偶然最佳值。
- WiFi 測試要記錄 RSSI；S3 連續重連要有 1–2 秒 pause。
- 測試 firmware 的背景 test tasks 必須記錄有冇停用。

## 4. 結果格式

```json
{
  "schema": 1,
  "date": "2026-11-15",
  "boards": {"P4": {"ip": "10.10.1.50", "psram_mb": 32},
             "S3": {"ip": "10.10.1.31", "psram_mb": 8}},
  "tests": [
    {"id": "NC4-P4-8K-TCP", "type": "oneway_nc4", "board": "P4",
     "payload": 8000, "batch": 10, "result": {"mbps": 5.55},
     "valid": true, "notes": "test build, background off"}
  ]
}
```

## 5. 執行前檢查

```sh
python3 make.py check ESP32-P4-ETH
python3 make.py check ESP32-S3-N16R8
curl -s -o /dev/null -w '%{http_code}\n' http://10.10.1.50/
curl -s -o /dev/null -w '%{http_code}\n' http://10.10.1.31/
```

- test build 要用 `--fresh --tests`，並確認 `firmware/build/CMakeCache.txt` 的
  `NC_ENABLE_TESTS:BOOL=ON`；否則 0xF040 不會存在。
- raw endpoint 要 `network.json rawbench=1`；測試完要關閉。
- 量 raw 時 source buffer 要記錄 SRAM／PSRAM、core、priority。

## 6. 目前結果

- 有效：見 `net-results-2026-11-15.json`
- 無效／未完成：見 `net-test-matrix-2026-11-15.md` §5
- 特別是：
  - P4 raw 單 stream 8.05–8.53 MB/s
  - P4 raw 雙 stream 曾見 11.787 MB/s，但未穩定重複
  - 舊 MPY raw 9.35 MB/s（不同 firmware／host）
  - NC4 今次最高 5.6 MB/s
