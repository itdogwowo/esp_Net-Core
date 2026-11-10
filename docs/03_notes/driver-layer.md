# 驅動與匯流排層（`driver/` / `bus/`）

> 2026-11-10 起。**用途**：把「硬體怎麼操作」從 `main.c` 分出來，對照舊專案
> `mp_Net-Core/slave/driver/*_drv.py` 的分法。
> 目前的 `main.c` 同時做「組裝」與「硬體細節」，3,800 行裡有一半是 driver。

---

## 1. 三層對照（舊專案 → 現在）

| 舊專案（Python） | 現在（C） | 責任 |
|---|---|---|
| `slave/driver/*_drv.py` | `components/driver/drv_*/` | **匯流排／周邊**：install、read、write、腳位、baud |
| `slave/lib/hw/*.py`（PCA9685／XL9555／TFT） | （未需要）`components/hw/` | **具體晶片**：用 driver 去操作一顆晶片 |
| `slave/lib/sys/hw_manager.py` | `nc_task` 的 `claims[]` ＋ 裝配層 | **owner 帳本**：誰獨占哪條 bus／哪支腳 |

## 1.1 中間那層：`bus/`（統一線路 API，**唔綁任何協議**）

使用者 2026-11-10 修正：`bus` 不是「一條線的實作」，而是**統一的線路 API 層** ——
用家在大多數情況之下**唔需要知道底下是什麼總線**（就像舊專案 `lib/hw/TFT.py`：
上面只管畫，下面 SPI 或 I80 都一樣）。

**雙重保障**（使用者定案）：

| 保障 | 內容 |
|---|---|
| ① 統一 API | `nc_bus_read / write / ioctl` ＋ `nc_bus_name / kind` |
| ② 逃生門 | `nc_bus_native()` 拿回底層 driver 的原生 handle（`uart_port_t*` 之類），需要特殊能力時直接呼叫原生 API |

> 兩者都保留。**唔會為了「統一」而封印硬體能力**，也唔會強迫簡單用途去摸暫存器。

### 三層「誰知道什麼」

| 層 | 知道 | 不知道 |
|---|---|---|
| `driver/` | 自己那顆硬體的暫存器、時序、腳位、原生 API | 上層是 NC4、LED 抑或 LCD |
| `bus/` | 「一條線」的統一語義（開、讀、寫、ioctl、事件） | 底下是 UART、SPI、I80、USB 抑或 socket |
| `sys/nc4` | NC4 幀、parser、路由 | 幀是經 UART 還是 TCP 來的 |

### 關鍵：NC4 ⟂ 網絡協議

**NC4 是幀協議，唔應該同網絡協議綁定**（使用者 2026-11-10）。所以：

- `sys/nc4/` ＝ NC4 協議本體（decoder／parser／router／通道插槽）
- `bus/` 的 socket 線（TCP／WS／UDP）＝ **一條線**，NC4 只是它的用家之一
- 將來 LED 波形、LCD 面板像素同樣是「用家」，經 `bus/` 拿線，**唔關 NC4 事**

```
        sys/nc4（NC4 幀）        sys 其他（任務、記憶體、config）
              │                        │
              └────────┬───────────────┘
                       │  統一 API：open / read / write / ioctl
                    bus/            （＋ nc_bus_native() 逃生門）
                       │
              ┌────────┴────────┐
         driver/            socket 線（lwIP）
      UART／USB／SPI／I80     TCP／WS／UDP
```

### 現況

| 組件 | 狀態 | 內容 |
|---|---|---|
| `driver/drv_uart` | ✅ 已抽 | 原生 API：`nc_uart_drv_install/read/write/send` |
| `bus/bus_core` | ✅ **已建（2026-11-10）** | `nc_bus_t` ＋ ops 表 ＋ 統一 API ＋ `nc_bus_native()` |
| `bus/bus_uart` | ✅ 已建、**未接線** | UART adapter（`nc_bus_uart_open`），main 仍然直接用 drv |
| `bus/bus_net` | ⚠️ 暫放 | TCP／WS／UDP／HTTP（原 `sys/transport`）；下一步應改成 socket 線的 adapter |
| `sys/nc4` | 不變 | NC4 協議本體（含 `nc4_bus.c` 的插槽與 parser） |
| `driver/drv_lcd`／`drv_led` | ⏸ 未來 | 將來裝在這裡，對外經 `bus` |

**邊界一句話**：

> driver **唔知道** NC4、路由、任務、config；它只回答「這條 bus 怎麼讀、怎麼寫」。
> 誰用、什麼時候用、腳位衝不衝突 —— 全部是**組裝層（`main.c`）**的責任。

---

## 2. 現況

| 組件 | 狀態 | 內容 | 風險 |
|---|---|---|---|
| `driver/drv_uart` | ✅ **已抽出（2026-11-10）** | `nc_uart_drv_install/read/write/send/ready`；~120 行 | 低（本板未開，行為不變） |
| `driver/drv_usb` | ⏸ 未抽 | TinyUSB 安裝、`cdc_read`／`cdc_send`、RX callback、`cdc.rx` 任務 | **高**（熱路徑、剛調好） |
| `driver/drv_sd` | ⏸ 未抽 | sdmmc 掛載／卸載（FAT 擁有者仍留在 `sys/store`） | 中（開機依賴） |
| `driver/drv_eth` | ⏸ 未抽 | `esp_eth` ＋ `esp_netif`（P4 才有） | 低（S3 不編） |
| `driver/drv_i2c` / `drv_spi` / `drv_i80` | ⏸ 未有 | 等真的有晶片／面板再接 | — |
| `bus/bus_net` | ✅ 已搬 | TCP／WS／UDP／HTTP（原 `sys/transport`） | 低（S3 只用 json） |
| `bus/bus_nc4` | ⏸ 未搬 | 匯流排框架 ＋ 出口表（原 `sys/nc4`） | 中（熱路徑） |

---

## 3. 規則（新 driver 要守）

1. **一個周邊一個 component**，名字 `drv_<peripheral>`（唔用裸 `uart`／`spi`，避免同 IDF 撞名）。
2. **對外只露一支 header**（`include/nc_<periph>_drv.h`），API 用「install / ready / read / write」四個動詞。
3. **唔 include** `nc4.h`、`nc_cfg.h`、`nc_task.h`；需要參數就由呼叫端用 cfg struct 傳入。
4. **政策留組裝層**：例如「GPIO7 已經被 SD 的 CLK 佔用就唔啟動 UART」這條**留在 `main.c`**，
   driver 只管「安裝成唔成功」。
5. **阻塞語義要寫清楚**：`read` 是「等第一個 byte 再搬走現有的」、
   `write` 是「阻塞到整幀排入 driver 環」。這兩句直接寫在 header 上（見 `drv_uart`）。
6. **driver 唔配記憶體**：環由 IDF driver 自己 allocate（見 [buffers.md §11](buffers.md) 的 TX 環）。

---

## 4. 為什麼要切（不是為了漂亮）

| 問題 | 現在 |
|---|---|
| `main.c` 3,793 行，一半是硬體細節 | 抽 UART 之後 −100 行（含新 cfg） |
| 改 UART 要喺 `main.c` 裡面搵 | 一個 `driver/drv_uart/` 就係全部 |
| 測試／模擬冇辦法換 driver | 有 header 就可以做 stub（對照 hosttest 的 stub 做法） |
| 新板／新腳位 | 只改組裝層的 cfg，driver 不動 |

---

## 5. 遷移順序（建議）

| 步 | 抽邊個 | 為什麼排這裡 |
|---|---|---|
| ✅ 1 | **UART** | 最細、最獨立；本板未開 → 零回歸風險（已做） |
| 2 | **SD** | 邊界清楚（掛載／卸載）；但開機依賴它，要 A/B 開機時間與 FAT 行為 |
| 3 | **ETH**（P4） | S3 不編，風險低；但只有在 P4 才驗得到 |
| 4 | **USB／CDC** | 熱路徑、剛調好（`kick_us` 42 µs）；**最後做**，而且要重跑整套 RTT／burst |

---

## 6. 已知的「仲未歸位」

- `main.c` 仍有 **ETH、SD、USB、GPIO 測試、I80 測試** 的硬體碼。
- `sys/store` 同時管 FAT 與 USB MSC —— **FAT 的擁有者**屬於 store，但「USB 裝置怎麼裝」屬於 `drv_usb`；
  切的時候要保住「一個 owner」這條（見 [fat-ownership.md](fat-ownership.md)）。
