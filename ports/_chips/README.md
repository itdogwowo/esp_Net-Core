# 晶片 profile（build-time）

同一顆 IC 的「常識」放這裡；板子的 `driver.json` 只寫 `profile`＋板級差異
（bus index、enable、GPIO label）。

```json
"TFT": {"enable": 1, "profile": "ST7789", "bus": {"type": "spi", "index": 0}}
```

合併次序（目標）：`_chips/<profile>.json` → 板子 `driver.json` 覆蓋 → runtime。
目前 firmware 只有 **`LED` 段**會經 `tools/ncportgen.py` 被讀（其餘段仍未讀）；
`tools/board.py check` 會驗 `profile` 指到的檔案存在。
