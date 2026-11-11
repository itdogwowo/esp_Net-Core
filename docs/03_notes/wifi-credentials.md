# Wi-Fi 憑證管理（SSID／Password，最多 10 組）

> 2026-11-10。參考舊專案 `slave/lib/sys/network_manager.py` 的行為移植到 C。
> 目的：唔需要每次重燒／改 config 才可以換 Wi-Fi；掃描到已知 SSID 就自動連。

---

## 1. 舊專案行為（要保留的）

| 行為 | 舊專案 |
|---|---|
| 數量 | `CRED_MAX = 10` |
| 儲存 | btree／NVS `wifi_credentials`（**唔放 config**） |
| 更新 | 已存在 → 更新密碼、移到最尾（最近使用） |
| 新增 | 未滿 → 加尾；滿 → 丟最舊（index 0） |
| config 相容 | `Network.wifi.ssid`＋`ssid_pw` 仍可作預設／hidden |
| 掃描 | 掃 3 次、按 RSSI 排序 |
| 連線順序 | ① 掃描到的已存 SSID → ② 預設 SSID（hidden 都試）→ ③ 盲連其餘已存 SSID |
| 成功 | `save_wifi_credential()`；舊專案會移到最尾 |
| 失敗 | 保留 STA 並持續重試；舊專案另外起 AP fallback |

---

## 2. 現在的 C 實作

檔案：`firmware/main/nc_wifi.c`、`firmware/main/nc_wifi.h`

### NVS

- namespace：`nccred`（刻意不用 `wifi`，避免撞到 IDF 的 Wi-Fi NVS）
- key：`creds`
- blob：magic／version／count＋10 組 `{ssid[33], pw[65]}`
- 只存 SSID＋密碼；**不對外輸出密碼**。

### API

```c
int       nc_wifi_cred_count(void);
int       nc_wifi_cred_ssids(char out[][33], int max);
esp_err_t nc_wifi_cred_add(const char *ssid, const char *pw);
esp_err_t nc_wifi_cred_remove(const char *ssid);
esp_err_t nc_wifi_cred_clear(void);
size_t    nc_wifi_cred_json(char *buf, size_t cap);   // count + ssids
```

### 啟動流程

```text
nc_wifi_boot()
  → 讀 network.json（wifi.enable / mode / ssid / ssid_pw / credentials[]）
  → cred_load_nvs()
  → config 的 ssid／credentials 加入記憶體（未即時寫 NVS）
  → 冇任何 SSID → off_no_cred（0 heap）
  → 有 → esp_wifi_init / start
  → 建立 wifi_mgr task（core1, prio 3, stack 6144）
```

`wifi_mgr`：

1. 掃描（`esp_wifi_scan_start(..., true)`），RSSI 由強到弱。
2. 掃描到的 AP 逐個對已存憑證；配對到就 `try_connect()`。
3. 掃描不到的（hidden）／其餘憑證 → 盲連。
4. 成功：`cred_add_mem(..., persist=true)`（移到最尾、寫 NVS）。
5. 失敗：每 30 秒重試；`0xF038` 新增憑證會叫醒它。

`try_connect()`：`esp_wifi_set_config` → `esp_wifi_connect` → 等 `GOT_IP`／`DISCONNECTED`，timeout 15 秒。

### 狀態

`STATUS keys=wifi` 新增：

```json
{"compiled":1,"state":"on","enabled":1,"ssid":"...","ip":1,
 "rssi":-45,"cred":2,"scanning":0,"connected":1}
```

不含密碼；`cred` 是已存組數。

---

## 3. Config 風格（兼容舊專案）

`network.json` 的 `wifi` 段支援：

```json
"wifi": {
  "enable": 1,
  "mode": "sta",
  "ssid": "",
  "ssid_pw": "",
  "credentials": [
    { "ssid": "my-ap", "pw": "..." }
  ]
}
```

- `ssid_pw` 為主；兼容 `password_pw`／`password`。
- `credentials[]` 是初始清單（舊專案 `_load_credentials` 的 fallback）；
  成功連線後寫入 NVS，之後以 NVS 為準。
- **編譯進去的 wiring／driver 是預設**，不是鎖死：
  用戶的 `data/config/hardware.json`（以及之後的 `driver.json`）可以覆蓋；
  目前 runtime 只接了 hardware 的 ETH／SDIO typed 段，driver 覆蓋仍是下一刀。

---

## 4. 測試指令

`0xF038 WIFI_CRED`：

| payload | 動作 |
|---|---|
| `[0]` | list → `{"count":N,"ssids":[...]}` |
| `[1][ssid_len][ssid][pw_len][pw]` | add／update（最多 10） |
| `[2][ssid_len][ssid]` | remove |
| `[3]` | clear |

只回 SSID，不回密碼。

---

## 5. 上板結果（S3）

- **B 板**：add／list／remove／clear、重開機後仍在（NVS persistence）✅。
- **A 板**：`wifi.enable=1`、NVS 只有一組憑證；開機後 manager 掃描→配對→連上：
  `{"state":"on","enabled":1,"ssid":"TestAP","ip":1,"rssi":-65,"cred":1,"scanning":0,"connected":1}`
- 這證明 **掃描配對＋自動連線** 路徑已通。
- 過程修了兩個 reset：
  1. NVS namespace `wifi` → `nccred`（避免撞 IDF）。
  2. `wifi_ap_record_t aps[32]` 由 `wifi_mgr` task stack 移到 BSS（約 3 KB），stack 4096→6144。

---

## 6. 未完成／要注意

| 項目 | 狀態 |
|---|---|
| 自動掃描配對、連線 | ✅ 上板已通（A 板，TestAP） |
| AP fallback（STA 失敗起 AP） | ⏸ 未移植 |
| `wifi.mode = ap / apsta` | ⏸ 仍只支援 sta |
| 多組憑證的 priority／RSSI 加權 | 目前按掃描 RSSI，不是每組憑證自己的偏好 |
| NVS 憑證加密 | ❌ 目前明文 blob；舊專案 btree 也是明文，之後可考慮 NVS encryption |
| driver.json runtime 覆蓋 | ⏸ 未接 |

**板子狀態**：A 已正常連上；B 測試後仍未重新列舉（需要 reset／replug）。
B 的 NVS 只留測試憑證，冇真憑證。
