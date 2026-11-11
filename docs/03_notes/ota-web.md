# 網絡 OTA 與網頁（v0）

> 2026-11-11。**已上板驗證（A 板）。**
> `/` 200、`/ota` 200；0.1.1 → 0.1.2 網絡 OTA 成功。

---

## 1. 網絡 OTA

- 同 WS／`/config.json` 共用 httpd（S3 port **8080**）。
- `POST /ota`
  - Body = raw `.bin`（不是 multipart）。
  - `esp_ota_begin(next_update_partition, content_len)`
  - 4 KB chunk `esp_ota_write`
  - `esp_ota_end` → `esp_ota_set_boot_partition`
  - 回 `{"ok":1,"reboot":1}`，約 800 ms 後 `esp_restart()`。
- 瀏覽器用 `fetch('/ota', {method:'POST', body:file})`；見首頁。
- 測試（新 image 上板後）：

```bash
curl --data-binary @firmware/build/esp_net_core.bin \
     http://<ip>:8080/ota
```

**安全**：目前冇 TLS、冇 token、冇簽章，只可以在可信 LAN 用。
正式對外之前一定要加 auth／HTTPS／signed image。

---

## 2. 網頁 v0

`GET /` 回一個最小 HTML：

- 顯示標題與「LAN only」警告
- 選 `.bin` → `POST /ota`
- `GET /config.json` 顯示目前 config 前 4000 B
- `/config.json` 本來就支援 GET／PUT

完整 UI（狀態、LED 預覽、WiFi 設定、路由、日誌）是**獨立任務**，不要塞進 v0。

---

## 3. 上板狀態

| 項目 | 狀態 |
|---|---|
| HTTP server 8080 | ✅ `/` 200 |
| `/ota` | ✅ POST 200，14.8 s（1.18 MB） |
| `/` | ✅ 200，991 B |
| 網絡 OTA 實測 | ✅ 0.1.1 → 0.1.2，app hash 改變、reboot 正常 |

- OTA 回覆要在 reboot 前送出；reboot delay 已由 800 ms 加到 **3 s**。
  第一次用 800 ms 時 curl timeout 但板子已完成 OTA；3 s 後回 200。
- 網絡 OTA 上傳期間 **HTTP 係 single-threaded**，其他 web/NC4 請求會等；
  正式版應放慢／分段或獨立 task。

---

## 4. 未完成

- 上板驗證 `/`、`/ota`、OTA 重啟後版本。
- OTA 進度（目前冇 progress，只有完成／失敗）。
- OTA auth／HTTPS／signed image。
- 完整 Web UI（獨立任務）。
- P4 的 web/OTA（P4 目前冇 WS httpd；要另外開 server）。
