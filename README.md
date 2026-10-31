# esp_Net-Core

Net-Core 的 C / ESP-IDF 版本（ESP-IDF v6.0.3）。

## 文件

入口：[docs/README.md](docs/README.md)（依「你是誰」分三類）

- **協議層** — [通道支援範圍](docs/01_protocol/transports.md)
- **使用教學** — [建置與燒錄](docs/02_guides/build_and_flash.md) · [測試方法與結果](docs/02_guides/testing.md)
- **筆記** — [重寫計劃](docs/03_notes/rewrite-plan.md) · [執行期流程](docs/03_notes/architecture-flow.md) · [component 佈局](docs/03_notes/component-layout.md) · [記憶體原語](docs/03_notes/memory.md) · [**緩衝管理器（設計）**](docs/03_notes/buffer-manager.md)

## 工具

```bash
tools/flash.sh      # 燒錄（需要一次 download mode）
tools/ota.py        # 之後都用這個更新，不需要按鍵
tools/nc4.py        # 送單一指令
tools/nc4_test.py   # 解碼器一致性測試
```

## 狀態

Phase A：USB composite（MSC + CDC）已通；NC4 解碼器 / 通道框架 / OTA / 記憶體原語已完成並在板上驗證。
下一步：緩衝管理器（[設計已定案](docs/03_notes/buffer-manager.md)，從 P0 開始）、OTA rollback、第二條 CDC（NC4 與 log 分離）、多通道轉接頭。

環境：ESP-IDF v6.0.3 已裝在 `~/esp/esp-idf`，`idf.py build` 通過（322 KB image）。安裝與踩雷記錄見
[build_and_flash.md](docs/02_guides/build_and_flash.md)。
