# `_chips/` — 晶片能力（build-time 事實）

這裡是 `CONFIG_IDF_TARGET` 那一層的資料：**核數**與**USB 能力**。
`tools/ncportgen.py` 讀 `board.json` 的 `chip`，再讀本目錄的 `<chip>.json`，
產生 `nc_cfg_chip_t`（`nc_port_chip()`）。

```jsonc
{
  "id": "esp32p4",
  "cores": 2,
  "usb": {
    "otg": true,
    "otg_hs": true,
    "serial_jtag": true,
    "shared_phy": false,
    "link": "usb_jtag",   // 出廠預設 USB 通道名；null = 這顆沒有 USB
    "kind": "usb_jtag",   // cdc / usb_jtag
    "rx_region": 4128
  }
}
```

- `link` 是「缺 system.json 時，第一次開機用哪條 USB 線」；有 USJ 的晶片用
  `usb_jtag`，只有 OTG 的（S2）用 `CDC`，兩樣都沒有就 `null`。
- 新增晶片：加一個 JSON，`tools/board.py` 的 `CHIP_DIR` 也要加同一個 target。
- `firmware/components/sys/config/nc_cfg_chip.c` 仍然保留作 runtime fallback；
  正常 build 由 `nc_port_chip()` 提供，兩者不應該給出不同答案。
