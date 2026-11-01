# esp_Net-Core

Net-Core 的 C / ESP-IDF 版本（ESP-IDF v6.0.3）。

## 文件

入口：[docs/00_status.md](docs/00_status.md)（單頁現況）· [docs/README.md](docs/README.md)（依「你是誰」分三類）

- **協議層** — [通道支援範圍](docs/01_protocol/transports.md)
- **使用教學** — [建置與燒錄](docs/02_guides/build_and_flash.md) · [測試方法與結果](docs/02_guides/testing.md)
- **筆記** — [失敗處理](docs/03_notes/failure-policy.md) · [怎麼寫 Service](docs/03_notes/writing-a-service.md) · [收／解路徑架構](docs/03_notes/rx-architecture.md) · [重寫計劃](docs/03_notes/rewrite-plan.md) · [執行期流程](docs/03_notes/architecture-flow.md) · [component 佈局](docs/03_notes/component-layout.md) · [記憶體原語](docs/03_notes/memory.md) · [**緩衝管理器（設計）**](docs/03_notes/buffer-manager.md) · [**燈效語言與 ABI（研究）**](docs/03_notes/effect-language.md) · [**擴充架構（研究）**](docs/03_notes/extension-architecture.md)

## 工具

```bash
tools/board.sh      # 板子切換 / 建置 / 燒錄 / monitor（s3 | p4）
tools/flash.sh      # 燒錄（需要一次 download mode）
tools/ota.py        # 之後都用這個更新，不需要按鍵
tools/nc4.py        # 送單一指令（序列埠）
tools/nc4_test.py   # 解碼器一致性測試
tools/selftest.py   # 一鍵自測（STATUS / 解碼 / OTA）
tools/nc4_tcp.py    # NC4 over TCP 吞吐與延遲（P4 乙太網，port 6000）
tools/net_bench.py  # 純 TCP 吞吐（port 5000/5001/5002）
tools/hub_bench.py  # 0xF003 雙核 hub 交換量測
tools/nc4_tx_bench.py  # 0xF007 上傳（打包＋送出）量測，同核/分核
tools/nc4_stats.py     # 0xF008 匯流排統計（copied 應為 0）+ 碎片/垃圾邊界測試
tools/ws_nc4_test.py   # NC4 over WebSocket 全套測試（RTT / 吞吐 / 碎片 / 重連 / 並存）
tools/udp_nc4_test.py  # NC4 over UDP（RTT / 吞吐 / 掉包）
tools/fx_poc/       # 效果模組 ABI 的 host 參考實作（node，零依賴，不需要板子）
```

## 狀態

Phase A：USB composite（MSC + CDC）已通；NC4 解碼器 / 通道框架 / OTA / 記憶體原語已完成並在板上驗證。

ESP32-P4 乙太網：純 TCP 7.4 / 7.1 MB/s；NC4 over TCP 在 PUSH 收斂模式（RX core1、解析 core0）
**6.0–6.4 MB/s**（1K/4K/8K 差距 < 3%），跨核 hub 交換 **2.0 µs/次、零撕裂**。
量測與踩雷見 [p4-bringup.md](docs/03_notes/p4-bringup.md)、[testing.md](docs/02_guides/testing.md)。

下一步：真零複製（parser 直接在插槽上解析）、PUSH 長時 soak、OTA rollback、
第二條 CDC（NC4 與 log 分離）、多通道轉接頭。

環境：ESP-IDF v6.0.3 已裝在 `~/esp/esp-idf`，`idf.py build` 通過（≈560 KB image）。安裝與踩雷記錄見
[build_and_flash.md](docs/02_guides/build_and_flash.md)。
