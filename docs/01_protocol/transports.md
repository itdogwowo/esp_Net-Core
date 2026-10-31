# 通道（Transport）支援範圍

> 原則：**解碼器只看「有序的位元組」**。底下是 USB、UART、CAN 還是 I2C，它不知道也不該知道。
> 分段、時序、重試都屬於 transport。

---

## 1. 兩種驅動方式

| 模式 | 誰主動 | 適用 |
|---|---|---|
| **PULL** | 解碼任務呼叫 `read()` | 有 RX 佇列、隨時可讀：USB CDC、UART、TCP/WS |
| **PUSH** | transport 呼叫 `nc4_bus_push()` | 事件式：I2C/SPI slave、CAN RX、ESP-NOW |

兩者共用同一組 `nc4_parser` / `nc4_decoder`，差別只在「位元組怎麼進來」。

註冊時決定一次，之後不可改：

```c
const nc4_bus_cfg_t cfg = {
    .name = "CAN0",
    .mode = NC4_BUS_PUSH,
    .send = can_send,
    .rx_ring_size = 4096,   // 0 = 框架預設 2048
};
nc4_bus_handle_t can;
nc4_bus_add(decoder, &cfg, &can);
```

---

## 2. 各通道要自己做什麼

| 通道 | 角色 | 一次搬多少 | 模式 | 回覆時機 | transport 必須自己做的 |
|---|---|---|---|---|---|
| **USB CDC** | slave | 串流 | PULL | 任意 | 無（已接） |
| **UART / RS485** | peer | 串流 | PULL | 任意 | 無（DE 腳位屬 driver） |
| **TCP / WS** | peer | 串流 | PULL | 任意 | 無 |
| **ESP-NOW** | peer | 250 B/則 | PUSH | 任意 | 分段／重組 |
| **I2C slave** | slave | 一次 transaction | PUSH | **同一 transaction 內** | 預先備好 TX + 跨 transaction 重組 |
| **SPI slave** | slave | ≤ DMA 段 | PUSH | **同一 transaction 內** | 同上，DMA done → push |
| **CAN (TWAI)** | peer | 8 B（FD 64 B） | PUSH | 任意 | **ISO-TP 分段／重組** |
| **I2C / SPI / CAN master** | master | — | PULL（寫後讀） | 需 timeout | 請求排隊、逾時、重試 |

### 為什麼 CAN 一定要分段

NC4 一幀可以到 8192 bytes，CAN 一則訊息只有 8 bytes。**差兩個數量級。**

所以 CAN 上的 NC4 不是「把幀塞進一則訊息」，而是：

```
NC4 幀 → ISO-TP（或自訂 chunk header）→ N 則 CAN 訊息 → 對端重組 → NC4 parser
```

這段必須在 transport 裡做完，push 進來的永遠是「已還原的連續位元組」。好處是：CAN 上的 NC4 與 USB 上的 NC4 是同一套指令、同一套 handler、同一個 CRC。

### 為什麼 slave 通道的回覆時機不一樣

I2C/SPI slave 沒有「等一下再回」這種事 —— master 的時鐘已經在跑了。所以：

- handler 直接 `ctx->send()` 的內容必須**先進一顆 TX buffer**，transport 在下一次 transaction 把它推出去；
- 如果 handler 是慢的（例如要寫 flash），那一次 transaction 只能先回「還沒好」，由 master 重試。

這一點在 PULL 通道（USB/UART）不存在，所以 `nc4_send_fn` 的實作要各自處理。

---

## 3. 加一條通道的成本

1. 寫兩個函式（`read` 或 `push` / `send`）
2. 一個 `nc4_bus_cfg_t`
3. `nc4_bus_add()`

解碼器、dispatch 表、所有 handler **一行都不用改**。這就是「所有通道」在架構上的意思：不是每個通道寫一套解碼，是每個通道寫一個轉接頭。

---

## 4. 目前的狀態

| 通道 | 狀態 |
|---|---|
| USB CDC | ✅ 已接（PULL） |
| 其他 | 框架已支援，缺各自的轉接頭 |
