# 可以設定什麼、在哪裡設、怎樣生效

> **用途**：你要改一個值之前，這一頁回答三件事：**它在哪一層**、**改完要 rebuild 定 reboot**、
> **用咩驗**。設計理由見 [config-layers.md](../03_notes/config-layers.md)；
> schema registry 契約見 [state-schema.md §8](../03_notes/state-schema.md)。
>
> **driver 微調**：[`driver-config.md`](driver-config.md)（LED 10 個 key、實際 parser 範圍、範例、陷阱）
> ｜**中央基準實際值**：[`ports/presets/STATUS.md`](../../ports/presets/STATUS.md)（生成）
> ｜**值會佔幾多**：[`state-storage-budget.md`](../03_notes/state-storage-budget.md)（生成；26 個 value key = 174–209 B blob）
> ｜圖：[設定流轉與權威層](../00_pm/config-layers.svg)｜
> [schema registry：可以設定什麼](../00_pm/schema-registry.svg)（兩張都由
> `python3 tools/config_diagram.py` 生成）

---

## 0. 一句話

```text
build 事實（board.json／sdkconfig／wiring）───┐
build 預置（ports/project/.../data/config）──┼─→ B02 nc_cfg_boot() ─→ cfg struct
FAT live（/data/config/*.json，最高權威）───┘                          │
                                                                      ├─→ 消費者（net／tasks／render／activation）
state.ncst（稀疏快取，可重建）← nc_state_boot_sync() ← 派生自上面      │
        └─→ RAM store ─→ nc_state_api（get／set，schema 驗證）────────┘
```

**權威鏈**：FAT live > build 預置 > 出廠 const（L0）。
**state.ncst 係快取**，任何時候都可以由前兩者重建，唔係第二個真相。

---

## 1. schema 覆蓋的設定（唯一名單）

> 下表由 `python3 tools/gen_state_keys.py --doc` 生成；三份生成檔＋本表同
> `schema/state-keys.json` 唔同步，`tools/hosttest/run_all.sh` 會直接失敗。
> 所以「有哪些值可以設定」永遠只有一個答案。

<!-- BEGIN GENERATED: schema -->
**現時：schema v2、286 個 key、16 個 unit（sys 4／net 3／link 64／route 8／rmt 32／pcnt 12／i80 5／spi 7／adc 60／twai 15／mcpwm 12／led 10／parlio 6／lcd 12／touch 28／cam 8）**

| unit | key（C 名） | type | class | 可接受值 | config 路徑（人讀） | 生效 |
|---|---|---|---|---|---|---|
| sys | `NC_KEY_SYS_CID` | u16 | value | 0..65535 | `system.json:cID` | reboot |
| sys | `NC_KEY_SYS_FRAME_MS` | u16 | value | 1..1000 | `system.json:frame_interval_ms` | reboot |
| sys | `NC_KEY_SYS_NUM_PIXELS` | u16 | value | 0..65535 | `system.json:num_pixels` | reboot |
| sys | `NC_KEY_SYS_LOG_DEBUG` | u8 | value | 0..1 | `system.json:log_debug_mode` | reboot |
| net | `NC_KEY_NET_WIFI_ENABLE` | bool | struct | 0..1 | `network.json:wifi.enable` | reboot |
| net | `NC_KEY_NET_WIFI_TIMEOUT` | u16 | struct | 1..600 | `network.json:wifi.timeout_s` | reboot |
| net | `NC_KEY_NET_WIFI_MODE` | enum | struct | enum：sta／ap | `network.json:wifi.mode` | reboot |
| link | `NC_KEY_LINK0_ENABLE` | bool | struct | 0..1 | `system.json:nc4.links[0].enable` | reboot |
| link | `NC_KEY_LINK0_KIND` | enum | struct | enum：usb／cdc／uart／tcp／udp／ws／http／usb_jtag | `system.json:nc4.links[0].kind` | reboot |
| link | `NC_KEY_LINK0_INDEX` | u8 | struct | 0..15 | `system.json:nc4.links[0].index` | reboot |
| link | `NC_KEY_LINK0_MSC` | i8 | struct | -1..1 | `system.json:nc4.links[0].msc` | reboot |
| link | `NC_KEY_LINK0_PORT` | u16 | struct | 0..65535 | `system.json:nc4.links[0].port` | reboot |
| link | `NC_KEY_LINK0_IFACE` | enum | struct | enum：any／eth／wifi | `system.json:nc4.links[0].iface` | reboot |
| link | `NC_KEY_LINK0_SVC` | enum | struct | enum：auto／nc4／web | `system.json:nc4.links[0].service` | reboot |
| link | `NC_KEY_LINK0_MEM` | enum | struct | enum：auto／sram／psram | `system.json:nc4.links[0].mem` | reboot |
| link | `NC_KEY_LINK1_ENABLE` | bool | struct | 0..1 | `system.json:nc4.links[1].enable` | reboot |
| link | `NC_KEY_LINK1_KIND` | enum | struct | enum：usb／cdc／uart／tcp／udp／ws／http／usb_jtag | `system.json:nc4.links[1].kind` | reboot |
| link | `NC_KEY_LINK1_INDEX` | u8 | struct | 0..15 | `system.json:nc4.links[1].index` | reboot |
| link | `NC_KEY_LINK1_MSC` | i8 | struct | -1..1 | `system.json:nc4.links[1].msc` | reboot |
| link | `NC_KEY_LINK1_PORT` | u16 | struct | 0..65535 | `system.json:nc4.links[1].port` | reboot |
| link | `NC_KEY_LINK1_IFACE` | enum | struct | enum：any／eth／wifi | `system.json:nc4.links[1].iface` | reboot |
| link | `NC_KEY_LINK1_SVC` | enum | struct | enum：auto／nc4／web | `system.json:nc4.links[1].service` | reboot |
| link | `NC_KEY_LINK1_MEM` | enum | struct | enum：auto／sram／psram | `system.json:nc4.links[1].mem` | reboot |
| link | `NC_KEY_LINK2_ENABLE` | bool | struct | 0..1 | `system.json:nc4.links[2].enable` | reboot |
| link | `NC_KEY_LINK2_KIND` | enum | struct | enum：usb／cdc／uart／tcp／udp／ws／http／usb_jtag | `system.json:nc4.links[2].kind` | reboot |
| link | `NC_KEY_LINK2_INDEX` | u8 | struct | 0..15 | `system.json:nc4.links[2].index` | reboot |
| link | `NC_KEY_LINK2_MSC` | i8 | struct | -1..1 | `system.json:nc4.links[2].msc` | reboot |
| link | `NC_KEY_LINK2_PORT` | u16 | struct | 0..65535 | `system.json:nc4.links[2].port` | reboot |
| link | `NC_KEY_LINK2_IFACE` | enum | struct | enum：any／eth／wifi | `system.json:nc4.links[2].iface` | reboot |
| link | `NC_KEY_LINK2_SVC` | enum | struct | enum：auto／nc4／web | `system.json:nc4.links[2].service` | reboot |
| link | `NC_KEY_LINK2_MEM` | enum | struct | enum：auto／sram／psram | `system.json:nc4.links[2].mem` | reboot |
| link | `NC_KEY_LINK3_ENABLE` | bool | struct | 0..1 | `system.json:nc4.links[3].enable` | reboot |
| link | `NC_KEY_LINK3_KIND` | enum | struct | enum：usb／cdc／uart／tcp／udp／ws／http／usb_jtag | `system.json:nc4.links[3].kind` | reboot |
| link | `NC_KEY_LINK3_INDEX` | u8 | struct | 0..15 | `system.json:nc4.links[3].index` | reboot |
| link | `NC_KEY_LINK3_MSC` | i8 | struct | -1..1 | `system.json:nc4.links[3].msc` | reboot |
| link | `NC_KEY_LINK3_PORT` | u16 | struct | 0..65535 | `system.json:nc4.links[3].port` | reboot |
| link | `NC_KEY_LINK3_IFACE` | enum | struct | enum：any／eth／wifi | `system.json:nc4.links[3].iface` | reboot |
| link | `NC_KEY_LINK3_SVC` | enum | struct | enum：auto／nc4／web | `system.json:nc4.links[3].service` | reboot |
| link | `NC_KEY_LINK3_MEM` | enum | struct | enum：auto／sram／psram | `system.json:nc4.links[3].mem` | reboot |
| link | `NC_KEY_LINK4_ENABLE` | bool | struct | 0..1 | `system.json:nc4.links[4].enable` | reboot |
| link | `NC_KEY_LINK4_KIND` | enum | struct | enum：usb／cdc／uart／tcp／udp／ws／http／usb_jtag | `system.json:nc4.links[4].kind` | reboot |
| link | `NC_KEY_LINK4_INDEX` | u8 | struct | 0..15 | `system.json:nc4.links[4].index` | reboot |
| link | `NC_KEY_LINK4_MSC` | i8 | struct | -1..1 | `system.json:nc4.links[4].msc` | reboot |
| link | `NC_KEY_LINK4_PORT` | u16 | struct | 0..65535 | `system.json:nc4.links[4].port` | reboot |
| link | `NC_KEY_LINK4_IFACE` | enum | struct | enum：any／eth／wifi | `system.json:nc4.links[4].iface` | reboot |
| link | `NC_KEY_LINK4_SVC` | enum | struct | enum：auto／nc4／web | `system.json:nc4.links[4].service` | reboot |
| link | `NC_KEY_LINK4_MEM` | enum | struct | enum：auto／sram／psram | `system.json:nc4.links[4].mem` | reboot |
| link | `NC_KEY_LINK5_ENABLE` | bool | struct | 0..1 | `system.json:nc4.links[5].enable` | reboot |
| link | `NC_KEY_LINK5_KIND` | enum | struct | enum：usb／cdc／uart／tcp／udp／ws／http／usb_jtag | `system.json:nc4.links[5].kind` | reboot |
| link | `NC_KEY_LINK5_INDEX` | u8 | struct | 0..15 | `system.json:nc4.links[5].index` | reboot |
| link | `NC_KEY_LINK5_MSC` | i8 | struct | -1..1 | `system.json:nc4.links[5].msc` | reboot |
| link | `NC_KEY_LINK5_PORT` | u16 | struct | 0..65535 | `system.json:nc4.links[5].port` | reboot |
| link | `NC_KEY_LINK5_IFACE` | enum | struct | enum：any／eth／wifi | `system.json:nc4.links[5].iface` | reboot |
| link | `NC_KEY_LINK5_SVC` | enum | struct | enum：auto／nc4／web | `system.json:nc4.links[5].service` | reboot |
| link | `NC_KEY_LINK5_MEM` | enum | struct | enum：auto／sram／psram | `system.json:nc4.links[5].mem` | reboot |
| link | `NC_KEY_LINK6_ENABLE` | bool | struct | 0..1 | `system.json:nc4.links[6].enable` | reboot |
| link | `NC_KEY_LINK6_KIND` | enum | struct | enum：usb／cdc／uart／tcp／udp／ws／http／usb_jtag | `system.json:nc4.links[6].kind` | reboot |
| link | `NC_KEY_LINK6_INDEX` | u8 | struct | 0..15 | `system.json:nc4.links[6].index` | reboot |
| link | `NC_KEY_LINK6_MSC` | i8 | struct | -1..1 | `system.json:nc4.links[6].msc` | reboot |
| link | `NC_KEY_LINK6_PORT` | u16 | struct | 0..65535 | `system.json:nc4.links[6].port` | reboot |
| link | `NC_KEY_LINK6_IFACE` | enum | struct | enum：any／eth／wifi | `system.json:nc4.links[6].iface` | reboot |
| link | `NC_KEY_LINK6_SVC` | enum | struct | enum：auto／nc4／web | `system.json:nc4.links[6].service` | reboot |
| link | `NC_KEY_LINK6_MEM` | enum | struct | enum：auto／sram／psram | `system.json:nc4.links[6].mem` | reboot |
| link | `NC_KEY_LINK7_ENABLE` | bool | struct | 0..1 | `system.json:nc4.links[7].enable` | reboot |
| link | `NC_KEY_LINK7_KIND` | enum | struct | enum：usb／cdc／uart／tcp／udp／ws／http／usb_jtag | `system.json:nc4.links[7].kind` | reboot |
| link | `NC_KEY_LINK7_INDEX` | u8 | struct | 0..15 | `system.json:nc4.links[7].index` | reboot |
| link | `NC_KEY_LINK7_MSC` | i8 | struct | -1..1 | `system.json:nc4.links[7].msc` | reboot |
| link | `NC_KEY_LINK7_PORT` | u16 | struct | 0..65535 | `system.json:nc4.links[7].port` | reboot |
| link | `NC_KEY_LINK7_IFACE` | enum | struct | enum：any／eth／wifi | `system.json:nc4.links[7].iface` | reboot |
| link | `NC_KEY_LINK7_SVC` | enum | struct | enum：auto／nc4／web | `system.json:nc4.links[7].service` | reboot |
| link | `NC_KEY_LINK7_MEM` | enum | struct | enum：auto／sram／psram | `system.json:nc4.links[7].mem` | reboot |
| route | `NC_KEY_ROUTE0_MASK` | u32 | struct | 0..65535 | `system.json:nc4.routes[0].out` | reboot |
| route | `NC_KEY_ROUTE1_MASK` | u32 | struct | 0..65535 | `system.json:nc4.routes[1].out` | reboot |
| route | `NC_KEY_ROUTE2_MASK` | u32 | struct | 0..65535 | `system.json:nc4.routes[2].out` | reboot |
| route | `NC_KEY_ROUTE3_MASK` | u32 | struct | 0..65535 | `system.json:nc4.routes[3].out` | reboot |
| route | `NC_KEY_ROUTE4_MASK` | u32 | struct | 0..65535 | `system.json:nc4.routes[4].out` | reboot |
| route | `NC_KEY_ROUTE5_MASK` | u32 | struct | 0..65535 | `system.json:nc4.routes[5].out` | reboot |
| route | `NC_KEY_ROUTE6_MASK` | u32 | struct | 0..65535 | `system.json:nc4.routes[6].out` | reboot |
| route | `NC_KEY_ROUTE7_MASK` | u32 | struct | 0..65535 | `system.json:nc4.routes[7].out` | reboot |
| rmt | `NC_KEY_RMT0_ENABLE` | bool | struct | 0..1 | `hardware.json:RMT.list[0].enable` | reboot |
| rmt | `NC_KEY_RMT0_GPIO` | i8 | struct | -1..54 | `hardware.json:RMT.list[0].gpio` | reboot |
| rmt | `NC_KEY_RMT0_MODE` | enum | struct | enum：tx／rx | `hardware.json:RMT.list[0].mode` | reboot |
| rmt | `NC_KEY_RMT0_DMA` | bool | struct | 0..1 | `hardware.json:RMT.list[0].dma` | reboot |
| rmt | `NC_KEY_RMT1_ENABLE` | bool | struct | 0..1 | `hardware.json:RMT.list[1].enable` | reboot |
| rmt | `NC_KEY_RMT1_GPIO` | i8 | struct | -1..54 | `hardware.json:RMT.list[1].gpio` | reboot |
| rmt | `NC_KEY_RMT1_MODE` | enum | struct | enum：tx／rx | `hardware.json:RMT.list[1].mode` | reboot |
| rmt | `NC_KEY_RMT1_DMA` | bool | struct | 0..1 | `hardware.json:RMT.list[1].dma` | reboot |
| rmt | `NC_KEY_RMT2_ENABLE` | bool | struct | 0..1 | `hardware.json:RMT.list[2].enable` | reboot |
| rmt | `NC_KEY_RMT2_GPIO` | i8 | struct | -1..54 | `hardware.json:RMT.list[2].gpio` | reboot |
| rmt | `NC_KEY_RMT2_MODE` | enum | struct | enum：tx／rx | `hardware.json:RMT.list[2].mode` | reboot |
| rmt | `NC_KEY_RMT2_DMA` | bool | struct | 0..1 | `hardware.json:RMT.list[2].dma` | reboot |
| rmt | `NC_KEY_RMT3_ENABLE` | bool | struct | 0..1 | `hardware.json:RMT.list[3].enable` | reboot |
| rmt | `NC_KEY_RMT3_GPIO` | i8 | struct | -1..54 | `hardware.json:RMT.list[3].gpio` | reboot |
| rmt | `NC_KEY_RMT3_MODE` | enum | struct | enum：tx／rx | `hardware.json:RMT.list[3].mode` | reboot |
| rmt | `NC_KEY_RMT3_DMA` | bool | struct | 0..1 | `hardware.json:RMT.list[3].dma` | reboot |
| rmt | `NC_KEY_RMT4_ENABLE` | bool | struct | 0..1 | `hardware.json:RMT.list[4].enable` | reboot |
| rmt | `NC_KEY_RMT4_GPIO` | i8 | struct | -1..54 | `hardware.json:RMT.list[4].gpio` | reboot |
| rmt | `NC_KEY_RMT4_MODE` | enum | struct | enum：tx／rx | `hardware.json:RMT.list[4].mode` | reboot |
| rmt | `NC_KEY_RMT4_DMA` | bool | struct | 0..1 | `hardware.json:RMT.list[4].dma` | reboot |
| rmt | `NC_KEY_RMT5_ENABLE` | bool | struct | 0..1 | `hardware.json:RMT.list[5].enable` | reboot |
| rmt | `NC_KEY_RMT5_GPIO` | i8 | struct | -1..54 | `hardware.json:RMT.list[5].gpio` | reboot |
| rmt | `NC_KEY_RMT5_MODE` | enum | struct | enum：tx／rx | `hardware.json:RMT.list[5].mode` | reboot |
| rmt | `NC_KEY_RMT5_DMA` | bool | struct | 0..1 | `hardware.json:RMT.list[5].dma` | reboot |
| rmt | `NC_KEY_RMT6_ENABLE` | bool | struct | 0..1 | `hardware.json:RMT.list[6].enable` | reboot |
| rmt | `NC_KEY_RMT6_GPIO` | i8 | struct | -1..54 | `hardware.json:RMT.list[6].gpio` | reboot |
| rmt | `NC_KEY_RMT6_MODE` | enum | struct | enum：tx／rx | `hardware.json:RMT.list[6].mode` | reboot |
| rmt | `NC_KEY_RMT6_DMA` | bool | struct | 0..1 | `hardware.json:RMT.list[6].dma` | reboot |
| rmt | `NC_KEY_RMT7_ENABLE` | bool | struct | 0..1 | `hardware.json:RMT.list[7].enable` | reboot |
| rmt | `NC_KEY_RMT7_GPIO` | i8 | struct | -1..54 | `hardware.json:RMT.list[7].gpio` | reboot |
| rmt | `NC_KEY_RMT7_MODE` | enum | struct | enum：tx／rx | `hardware.json:RMT.list[7].mode` | reboot |
| rmt | `NC_KEY_RMT7_DMA` | bool | struct | 0..1 | `hardware.json:RMT.list[7].dma` | reboot |
| pcnt | `NC_KEY_PCNT0_ENABLE` | bool | struct | 0..1 | `hardware.json:PCNT.list[0].enable` | reboot |
| pcnt | `NC_KEY_PCNT0_GPIO_A` | i8 | struct | -1..54 | `hardware.json:PCNT.list[0].gpio_a` | reboot |
| pcnt | `NC_KEY_PCNT0_GPIO_B` | i8 | struct | -1..54 | `hardware.json:PCNT.list[0].gpio_b` | reboot |
| pcnt | `NC_KEY_PCNT1_ENABLE` | bool | struct | 0..1 | `hardware.json:PCNT.list[1].enable` | reboot |
| pcnt | `NC_KEY_PCNT1_GPIO_A` | i8 | struct | -1..54 | `hardware.json:PCNT.list[1].gpio_a` | reboot |
| pcnt | `NC_KEY_PCNT1_GPIO_B` | i8 | struct | -1..54 | `hardware.json:PCNT.list[1].gpio_b` | reboot |
| pcnt | `NC_KEY_PCNT2_ENABLE` | bool | struct | 0..1 | `hardware.json:PCNT.list[2].enable` | reboot |
| pcnt | `NC_KEY_PCNT2_GPIO_A` | i8 | struct | -1..54 | `hardware.json:PCNT.list[2].gpio_a` | reboot |
| pcnt | `NC_KEY_PCNT2_GPIO_B` | i8 | struct | -1..54 | `hardware.json:PCNT.list[2].gpio_b` | reboot |
| pcnt | `NC_KEY_PCNT3_ENABLE` | bool | struct | 0..1 | `hardware.json:PCNT.list[3].enable` | reboot |
| pcnt | `NC_KEY_PCNT3_GPIO_A` | i8 | struct | -1..54 | `hardware.json:PCNT.list[3].gpio_a` | reboot |
| pcnt | `NC_KEY_PCNT3_GPIO_B` | i8 | struct | -1..54 | `hardware.json:PCNT.list[3].gpio_b` | reboot |
| i80 | `NC_KEY_I80_COUNT` | u8 | value | 0..16 | `hardware.json:I80.list[0].data` | reboot |
| i80 | `NC_KEY_I80_WR` | i8 | value | -1..54 | `hardware.json:I80.list[0].wr` | reboot |
| i80 | `NC_KEY_I80_DC` | i8 | value | -1..54 | `hardware.json:I80.list[0].dc` | reboot |
| i80 | `NC_KEY_I80_CS` | i8 | value | -1..54 | `hardware.json:I80.list[0].cs` | reboot |
| i80 | `NC_KEY_I80_FREQ` | u32 | value | 1000000..80000000 | `hardware.json:I80.list[0].freq` | reboot |
| spi | `NC_KEY_SPI_ENABLE` | bool | value | 0..1 | `hardware.json:SPI.enable` | reboot |
| spi | `NC_KEY_SPI_BAUD` | u32 | value | 100000..80000000 | `hardware.json:SPI.list[0].baudrate` | reboot |
| spi | `NC_KEY_SPI_PHASE` | u8 | value | 0..1 | `hardware.json:SPI.list[0].phase` | reboot |
| spi | `NC_KEY_SPI_POL` | u8 | value | 0..1 | `hardware.json:SPI.list[0].polarity` | reboot |
| spi | `NC_KEY_SPI_SCK` | i8 | value | -1..54 | `hardware.json:SPI.list[0].GPIO.sck` | reboot |
| spi | `NC_KEY_SPI_MOSI` | i8 | value | -1..54 | `hardware.json:SPI.list[0].GPIO.mosi` | reboot |
| spi | `NC_KEY_SPI_MISO` | i8 | value | -1..54 | `hardware.json:SPI.list[0].GPIO.miso` | reboot |
| adc | `NC_KEY_ADC0_ENABLE` | bool | struct | 0..1 | `hardware.json:ADC.list[0].enable` | reboot |
| adc | `NC_KEY_ADC0_UNIT` | enum | struct | enum：adc1／adc2 | `hardware.json:ADC.list[0].unit` | reboot |
| adc | `NC_KEY_ADC0_ATTEN` | enum | struct | enum：0db／2_5db／6db／12db | `hardware.json:ADC.list[0].atten` | reboot |
| adc | `NC_KEY_ADC1_ENABLE` | bool | struct | 0..1 | `hardware.json:ADC.list[1].enable` | reboot |
| adc | `NC_KEY_ADC1_UNIT` | enum | struct | enum：adc1／adc2 | `hardware.json:ADC.list[1].unit` | reboot |
| adc | `NC_KEY_ADC1_ATTEN` | enum | struct | enum：0db／2_5db／6db／12db | `hardware.json:ADC.list[1].atten` | reboot |
| adc | `NC_KEY_ADC2_ENABLE` | bool | struct | 0..1 | `hardware.json:ADC.list[2].enable` | reboot |
| adc | `NC_KEY_ADC2_UNIT` | enum | struct | enum：adc1／adc2 | `hardware.json:ADC.list[2].unit` | reboot |
| adc | `NC_KEY_ADC2_ATTEN` | enum | struct | enum：0db／2_5db／6db／12db | `hardware.json:ADC.list[2].atten` | reboot |
| adc | `NC_KEY_ADC3_ENABLE` | bool | struct | 0..1 | `hardware.json:ADC.list[3].enable` | reboot |
| adc | `NC_KEY_ADC3_UNIT` | enum | struct | enum：adc1／adc2 | `hardware.json:ADC.list[3].unit` | reboot |
| adc | `NC_KEY_ADC3_ATTEN` | enum | struct | enum：0db／2_5db／6db／12db | `hardware.json:ADC.list[3].atten` | reboot |
| adc | `NC_KEY_ADC4_ENABLE` | bool | struct | 0..1 | `hardware.json:ADC.list[4].enable` | reboot |
| adc | `NC_KEY_ADC4_UNIT` | enum | struct | enum：adc1／adc2 | `hardware.json:ADC.list[4].unit` | reboot |
| adc | `NC_KEY_ADC4_ATTEN` | enum | struct | enum：0db／2_5db／6db／12db | `hardware.json:ADC.list[4].atten` | reboot |
| adc | `NC_KEY_ADC5_ENABLE` | bool | struct | 0..1 | `hardware.json:ADC.list[5].enable` | reboot |
| adc | `NC_KEY_ADC5_UNIT` | enum | struct | enum：adc1／adc2 | `hardware.json:ADC.list[5].unit` | reboot |
| adc | `NC_KEY_ADC5_ATTEN` | enum | struct | enum：0db／2_5db／6db／12db | `hardware.json:ADC.list[5].atten` | reboot |
| adc | `NC_KEY_ADC6_ENABLE` | bool | struct | 0..1 | `hardware.json:ADC.list[6].enable` | reboot |
| adc | `NC_KEY_ADC6_UNIT` | enum | struct | enum：adc1／adc2 | `hardware.json:ADC.list[6].unit` | reboot |
| adc | `NC_KEY_ADC6_ATTEN` | enum | struct | enum：0db／2_5db／6db／12db | `hardware.json:ADC.list[6].atten` | reboot |
| adc | `NC_KEY_ADC7_ENABLE` | bool | struct | 0..1 | `hardware.json:ADC.list[7].enable` | reboot |
| adc | `NC_KEY_ADC7_UNIT` | enum | struct | enum：adc1／adc2 | `hardware.json:ADC.list[7].unit` | reboot |
| adc | `NC_KEY_ADC7_ATTEN` | enum | struct | enum：0db／2_5db／6db／12db | `hardware.json:ADC.list[7].atten` | reboot |
| adc | `NC_KEY_ADC8_ENABLE` | bool | struct | 0..1 | `hardware.json:ADC.list[8].enable` | reboot |
| adc | `NC_KEY_ADC8_UNIT` | enum | struct | enum：adc1／adc2 | `hardware.json:ADC.list[8].unit` | reboot |
| adc | `NC_KEY_ADC8_ATTEN` | enum | struct | enum：0db／2_5db／6db／12db | `hardware.json:ADC.list[8].atten` | reboot |
| adc | `NC_KEY_ADC9_ENABLE` | bool | struct | 0..1 | `hardware.json:ADC.list[9].enable` | reboot |
| adc | `NC_KEY_ADC9_UNIT` | enum | struct | enum：adc1／adc2 | `hardware.json:ADC.list[9].unit` | reboot |
| adc | `NC_KEY_ADC9_ATTEN` | enum | struct | enum：0db／2_5db／6db／12db | `hardware.json:ADC.list[9].atten` | reboot |
| adc | `NC_KEY_ADC10_ENABLE` | bool | struct | 0..1 | `hardware.json:ADC.list[10].enable` | reboot |
| adc | `NC_KEY_ADC10_UNIT` | enum | struct | enum：adc1／adc2 | `hardware.json:ADC.list[10].unit` | reboot |
| adc | `NC_KEY_ADC10_ATTEN` | enum | struct | enum：0db／2_5db／6db／12db | `hardware.json:ADC.list[10].atten` | reboot |
| adc | `NC_KEY_ADC11_ENABLE` | bool | struct | 0..1 | `hardware.json:ADC.list[11].enable` | reboot |
| adc | `NC_KEY_ADC11_UNIT` | enum | struct | enum：adc1／adc2 | `hardware.json:ADC.list[11].unit` | reboot |
| adc | `NC_KEY_ADC11_ATTEN` | enum | struct | enum：0db／2_5db／6db／12db | `hardware.json:ADC.list[11].atten` | reboot |
| adc | `NC_KEY_ADC12_ENABLE` | bool | struct | 0..1 | `hardware.json:ADC.list[12].enable` | reboot |
| adc | `NC_KEY_ADC12_UNIT` | enum | struct | enum：adc1／adc2 | `hardware.json:ADC.list[12].unit` | reboot |
| adc | `NC_KEY_ADC12_ATTEN` | enum | struct | enum：0db／2_5db／6db／12db | `hardware.json:ADC.list[12].atten` | reboot |
| adc | `NC_KEY_ADC13_ENABLE` | bool | struct | 0..1 | `hardware.json:ADC.list[13].enable` | reboot |
| adc | `NC_KEY_ADC13_UNIT` | enum | struct | enum：adc1／adc2 | `hardware.json:ADC.list[13].unit` | reboot |
| adc | `NC_KEY_ADC13_ATTEN` | enum | struct | enum：0db／2_5db／6db／12db | `hardware.json:ADC.list[13].atten` | reboot |
| adc | `NC_KEY_ADC14_ENABLE` | bool | struct | 0..1 | `hardware.json:ADC.list[14].enable` | reboot |
| adc | `NC_KEY_ADC14_UNIT` | enum | struct | enum：adc1／adc2 | `hardware.json:ADC.list[14].unit` | reboot |
| adc | `NC_KEY_ADC14_ATTEN` | enum | struct | enum：0db／2_5db／6db／12db | `hardware.json:ADC.list[14].atten` | reboot |
| adc | `NC_KEY_ADC15_ENABLE` | bool | struct | 0..1 | `hardware.json:ADC.list[15].enable` | reboot |
| adc | `NC_KEY_ADC15_UNIT` | enum | struct | enum：adc1／adc2 | `hardware.json:ADC.list[15].unit` | reboot |
| adc | `NC_KEY_ADC15_ATTEN` | enum | struct | enum：0db／2_5db／6db／12db | `hardware.json:ADC.list[15].atten` | reboot |
| adc | `NC_KEY_ADC16_ENABLE` | bool | struct | 0..1 | `hardware.json:ADC.list[16].enable` | reboot |
| adc | `NC_KEY_ADC16_UNIT` | enum | struct | enum：adc1／adc2 | `hardware.json:ADC.list[16].unit` | reboot |
| adc | `NC_KEY_ADC16_ATTEN` | enum | struct | enum：0db／2_5db／6db／12db | `hardware.json:ADC.list[16].atten` | reboot |
| adc | `NC_KEY_ADC17_ENABLE` | bool | struct | 0..1 | `hardware.json:ADC.list[17].enable` | reboot |
| adc | `NC_KEY_ADC17_UNIT` | enum | struct | enum：adc1／adc2 | `hardware.json:ADC.list[17].unit` | reboot |
| adc | `NC_KEY_ADC17_ATTEN` | enum | struct | enum：0db／2_5db／6db／12db | `hardware.json:ADC.list[17].atten` | reboot |
| adc | `NC_KEY_ADC18_ENABLE` | bool | struct | 0..1 | `hardware.json:ADC.list[18].enable` | reboot |
| adc | `NC_KEY_ADC18_UNIT` | enum | struct | enum：adc1／adc2 | `hardware.json:ADC.list[18].unit` | reboot |
| adc | `NC_KEY_ADC18_ATTEN` | enum | struct | enum：0db／2_5db／6db／12db | `hardware.json:ADC.list[18].atten` | reboot |
| adc | `NC_KEY_ADC19_ENABLE` | bool | struct | 0..1 | `hardware.json:ADC.list[19].enable` | reboot |
| adc | `NC_KEY_ADC19_UNIT` | enum | struct | enum：adc1／adc2 | `hardware.json:ADC.list[19].unit` | reboot |
| adc | `NC_KEY_ADC19_ATTEN` | enum | struct | enum：0db／2_5db／6db／12db | `hardware.json:ADC.list[19].atten` | reboot |
| twai | `NC_KEY_TWAI0_ENABLE` | bool | struct | 0..1 | `hardware.json:TWAI.list[0].enable` | reboot |
| twai | `NC_KEY_TWAI0_TX` | i8 | struct | -1..54 | `hardware.json:TWAI.list[0].tx` | reboot |
| twai | `NC_KEY_TWAI0_RX` | i8 | struct | -1..54 | `hardware.json:TWAI.list[0].rx` | reboot |
| twai | `NC_KEY_TWAI0_BITRATE` | u32 | struct | 10000..1000000 | `hardware.json:TWAI.list[0].bitrate` | reboot |
| twai | `NC_KEY_TWAI0_MODE` | enum | struct | enum：normal／listen | `hardware.json:TWAI.list[0].mode` | reboot |
| twai | `NC_KEY_TWAI1_ENABLE` | bool | struct | 0..1 | `hardware.json:TWAI.list[1].enable` | reboot |
| twai | `NC_KEY_TWAI1_TX` | i8 | struct | -1..54 | `hardware.json:TWAI.list[1].tx` | reboot |
| twai | `NC_KEY_TWAI1_RX` | i8 | struct | -1..54 | `hardware.json:TWAI.list[1].rx` | reboot |
| twai | `NC_KEY_TWAI1_BITRATE` | u32 | struct | 10000..1000000 | `hardware.json:TWAI.list[1].bitrate` | reboot |
| twai | `NC_KEY_TWAI1_MODE` | enum | struct | enum：normal／listen | `hardware.json:TWAI.list[1].mode` | reboot |
| twai | `NC_KEY_TWAI2_ENABLE` | bool | struct | 0..1 | `hardware.json:TWAI.list[2].enable` | reboot |
| twai | `NC_KEY_TWAI2_TX` | i8 | struct | -1..54 | `hardware.json:TWAI.list[2].tx` | reboot |
| twai | `NC_KEY_TWAI2_RX` | i8 | struct | -1..54 | `hardware.json:TWAI.list[2].rx` | reboot |
| twai | `NC_KEY_TWAI2_BITRATE` | u32 | struct | 10000..1000000 | `hardware.json:TWAI.list[2].bitrate` | reboot |
| twai | `NC_KEY_TWAI2_MODE` | enum | struct | enum：normal／listen | `hardware.json:TWAI.list[2].mode` | reboot |
| mcpwm | `NC_KEY_MCPWM0_ENABLE` | bool | struct | 0..1 | `hardware.json:MCPWM.list[0].enable` | reboot |
| mcpwm | `NC_KEY_MCPWM0_TIMER` | u8 | struct | 0..2 | `hardware.json:MCPWM.list[0].timer` | reboot |
| mcpwm | `NC_KEY_MCPWM0_GPIO_A` | i8 | struct | -1..54 | `hardware.json:MCPWM.list[0].gpio_a` | reboot |
| mcpwm | `NC_KEY_MCPWM0_GPIO_B` | i8 | struct | -1..54 | `hardware.json:MCPWM.list[0].gpio_b` | reboot |
| mcpwm | `NC_KEY_MCPWM0_FREQ` | u32 | struct | 1..40000000 | `hardware.json:MCPWM.list[0].freq` | reboot |
| mcpwm | `NC_KEY_MCPWM0_DUTY` | u16 | struct | 0..1000 | `hardware.json:MCPWM.list[0].duty` | reboot |
| mcpwm | `NC_KEY_MCPWM1_ENABLE` | bool | struct | 0..1 | `hardware.json:MCPWM.list[1].enable` | reboot |
| mcpwm | `NC_KEY_MCPWM1_TIMER` | u8 | struct | 0..2 | `hardware.json:MCPWM.list[1].timer` | reboot |
| mcpwm | `NC_KEY_MCPWM1_GPIO_A` | i8 | struct | -1..54 | `hardware.json:MCPWM.list[1].gpio_a` | reboot |
| mcpwm | `NC_KEY_MCPWM1_GPIO_B` | i8 | struct | -1..54 | `hardware.json:MCPWM.list[1].gpio_b` | reboot |
| mcpwm | `NC_KEY_MCPWM1_FREQ` | u32 | struct | 1..40000000 | `hardware.json:MCPWM.list[1].freq` | reboot |
| mcpwm | `NC_KEY_MCPWM1_DUTY` | u16 | struct | 0..1000 | `hardware.json:MCPWM.list[1].duty` | reboot |
| led | `NC_KEY_LED_ENABLE` | bool | value | 0..1 | `driver.json:LED.enable` | reboot |
| led | `NC_KEY_LED_BACKEND` | enum | value | enum：none／i80／spi／rmt／parlio／direct／mock | `driver.json:LED.backend` | reboot |
| led | `NC_KEY_LED_BUS` | i8 | value | -1..15 | `driver.json:LED.bus.index` | reboot |
| led | `NC_KEY_LED_LANES` | u8 | value | 1..16 | `driver.json:LED.lanes` | reboot |
| led | `NC_KEY_LED_PX` | u16 | value | 1..65535 | `driver.json:LED.px` | reboot |
| led | `NC_KEY_LED_FMT` | enum | value | enum：ws2812／ws2812w／ws2815／ws2801／apa102 | `driver.json:LED.format` | reboot |
| led | `NC_KEY_LED_CLK` | u32 | value | 0..80000000 | `driver.json:LED.timing.pclk_hz` | reboot |
| led | `NC_KEY_LED_BIT_CELLS` | u8 | value | 0..64 | `driver.json:LED.timing.bit_cells` | reboot |
| led | `NC_KEY_LED_T0H` | u8 | value | 0..64 | `driver.json:LED.timing.t0h` | reboot |
| led | `NC_KEY_LED_T1H` | u8 | value | 0..64 | `driver.json:LED.timing.t1h` | reboot |
| parlio | `NC_KEY_PARLIO0_ENABLE` | bool | struct | 0..1 | `hardware.json:PARLIO.list[0].enable` | reboot |
| parlio | `NC_KEY_PARLIO0_WIDTH` | u8 | struct | 0..16 | `hardware.json:PARLIO.list[0].data_width` | reboot |
| parlio | `NC_KEY_PARLIO0_CLK` | i8 | struct | -1..54 | `hardware.json:PARLIO.list[0].clk` | reboot |
| parlio | `NC_KEY_PARLIO0_D0` | i8 | struct | -1..54 | `hardware.json:PARLIO.list[0].data[0]` | reboot |
| parlio | `NC_KEY_PARLIO0_FREQ` | u32 | struct | 1000000..40000000 | `hardware.json:PARLIO.list[0].freq` | reboot |
| parlio | `NC_KEY_PARLIO0_MODE` | enum | struct | enum：tx／rx | `hardware.json:PARLIO.list[0].mode` | reboot |
| lcd | `NC_KEY_LCD0_ENABLE` | bool | struct | 0..1 | `hardware.json:LCD.list[0].enable` | reboot |
| lcd | `NC_KEY_LCD0_WIDTH` | u8 | struct | 0..24 | `hardware.json:LCD.list[0].data_width` | reboot |
| lcd | `NC_KEY_LCD0_PCLK` | i8 | struct | -1..54 | `hardware.json:LCD.list[0].pclk` | reboot |
| lcd | `NC_KEY_LCD0_HSYNC` | i8 | struct | -1..54 | `hardware.json:LCD.list[0].hsync` | reboot |
| lcd | `NC_KEY_LCD0_VSYNC` | i8 | struct | -1..54 | `hardware.json:LCD.list[0].vsync` | reboot |
| lcd | `NC_KEY_LCD0_DE` | i8 | struct | -1..54 | `hardware.json:LCD.list[0].de` | reboot |
| lcd | `NC_KEY_LCD0_FREQ` | u32 | struct | 1000000..80000000 | `hardware.json:LCD.list[0].freq` | reboot |
| lcd | `NC_KEY_LCD0_HPX` | u16 | struct | 0..4096 | `hardware.json:LCD.list[0].h_res` | reboot |
| lcd | `NC_KEY_LCD0_VPX` | u16 | struct | 0..4096 | `hardware.json:LCD.list[0].v_res` | reboot |
| lcd | `NC_KEY_LCD0_HSYNC_PW` | u16 | struct | 0..1024 | `hardware.json:LCD.list[0].hsync_pw` | reboot |
| lcd | `NC_KEY_LCD0_VSYNC_PW` | u16 | struct | 0..1024 | `hardware.json:LCD.list[0].vsync_pw` | reboot |
| lcd | `NC_KEY_LCD0_BIG_ENDIAN` | bool | struct | 0..1 | `hardware.json:LCD.list[0].big_endian` | reboot |
| touch | `NC_KEY_TOUCH0_ENABLE` | bool | struct | 0..1 | `hardware.json:TOUCH.list[0].enable` | reboot |
| touch | `NC_KEY_TOUCH0_THRESHOLD` | u16 | struct | 0..65535 | `hardware.json:TOUCH.list[0].threshold` | reboot |
| touch | `NC_KEY_TOUCH1_ENABLE` | bool | struct | 0..1 | `hardware.json:TOUCH.list[1].enable` | reboot |
| touch | `NC_KEY_TOUCH1_THRESHOLD` | u16 | struct | 0..65535 | `hardware.json:TOUCH.list[1].threshold` | reboot |
| touch | `NC_KEY_TOUCH2_ENABLE` | bool | struct | 0..1 | `hardware.json:TOUCH.list[2].enable` | reboot |
| touch | `NC_KEY_TOUCH2_THRESHOLD` | u16 | struct | 0..65535 | `hardware.json:TOUCH.list[2].threshold` | reboot |
| touch | `NC_KEY_TOUCH3_ENABLE` | bool | struct | 0..1 | `hardware.json:TOUCH.list[3].enable` | reboot |
| touch | `NC_KEY_TOUCH3_THRESHOLD` | u16 | struct | 0..65535 | `hardware.json:TOUCH.list[3].threshold` | reboot |
| touch | `NC_KEY_TOUCH4_ENABLE` | bool | struct | 0..1 | `hardware.json:TOUCH.list[4].enable` | reboot |
| touch | `NC_KEY_TOUCH4_THRESHOLD` | u16 | struct | 0..65535 | `hardware.json:TOUCH.list[4].threshold` | reboot |
| touch | `NC_KEY_TOUCH5_ENABLE` | bool | struct | 0..1 | `hardware.json:TOUCH.list[5].enable` | reboot |
| touch | `NC_KEY_TOUCH5_THRESHOLD` | u16 | struct | 0..65535 | `hardware.json:TOUCH.list[5].threshold` | reboot |
| touch | `NC_KEY_TOUCH6_ENABLE` | bool | struct | 0..1 | `hardware.json:TOUCH.list[6].enable` | reboot |
| touch | `NC_KEY_TOUCH6_THRESHOLD` | u16 | struct | 0..65535 | `hardware.json:TOUCH.list[6].threshold` | reboot |
| touch | `NC_KEY_TOUCH7_ENABLE` | bool | struct | 0..1 | `hardware.json:TOUCH.list[7].enable` | reboot |
| touch | `NC_KEY_TOUCH7_THRESHOLD` | u16 | struct | 0..65535 | `hardware.json:TOUCH.list[7].threshold` | reboot |
| touch | `NC_KEY_TOUCH8_ENABLE` | bool | struct | 0..1 | `hardware.json:TOUCH.list[8].enable` | reboot |
| touch | `NC_KEY_TOUCH8_THRESHOLD` | u16 | struct | 0..65535 | `hardware.json:TOUCH.list[8].threshold` | reboot |
| touch | `NC_KEY_TOUCH9_ENABLE` | bool | struct | 0..1 | `hardware.json:TOUCH.list[9].enable` | reboot |
| touch | `NC_KEY_TOUCH9_THRESHOLD` | u16 | struct | 0..65535 | `hardware.json:TOUCH.list[9].threshold` | reboot |
| touch | `NC_KEY_TOUCH10_ENABLE` | bool | struct | 0..1 | `hardware.json:TOUCH.list[10].enable` | reboot |
| touch | `NC_KEY_TOUCH10_THRESHOLD` | u16 | struct | 0..65535 | `hardware.json:TOUCH.list[10].threshold` | reboot |
| touch | `NC_KEY_TOUCH11_ENABLE` | bool | struct | 0..1 | `hardware.json:TOUCH.list[11].enable` | reboot |
| touch | `NC_KEY_TOUCH11_THRESHOLD` | u16 | struct | 0..65535 | `hardware.json:TOUCH.list[11].threshold` | reboot |
| touch | `NC_KEY_TOUCH12_ENABLE` | bool | struct | 0..1 | `hardware.json:TOUCH.list[12].enable` | reboot |
| touch | `NC_KEY_TOUCH12_THRESHOLD` | u16 | struct | 0..65535 | `hardware.json:TOUCH.list[12].threshold` | reboot |
| touch | `NC_KEY_TOUCH13_ENABLE` | bool | struct | 0..1 | `hardware.json:TOUCH.list[13].enable` | reboot |
| touch | `NC_KEY_TOUCH13_THRESHOLD` | u16 | struct | 0..65535 | `hardware.json:TOUCH.list[13].threshold` | reboot |
| cam | `NC_KEY_CAM0_ENABLE` | bool | struct | 0..1 | `hardware.json:CAM.list[0].enable` | reboot |
| cam | `NC_KEY_CAM0_WIDTH` | u8 | struct | 0..16 | `hardware.json:CAM.list[0].data_width` | reboot |
| cam | `NC_KEY_CAM0_PCLK` | i8 | struct | -1..54 | `hardware.json:CAM.list[0].pclk` | reboot |
| cam | `NC_KEY_CAM0_VSYNC` | i8 | struct | -1..54 | `hardware.json:CAM.list[0].vsync` | reboot |
| cam | `NC_KEY_CAM0_HREF` | i8 | struct | -1..54 | `hardware.json:CAM.list[0].href` | reboot |
| cam | `NC_KEY_CAM0_XCLK` | i8 | struct | -1..54 | `hardware.json:CAM.list[0].xclk` | reboot |
| cam | `NC_KEY_CAM0_XCLK_FREQ` | u32 | struct | 1000000..40000000 | `hardware.json:CAM.list[0].xclk_freq` | reboot |
| cam | `NC_KEY_CAM0_SCCB_ADDR` | u8 | struct | 0..127 | `hardware.json:CAM.list[0].sccb_addr` | reboot |

> 「生效」全部係 reboot：B02 由 JSON 重算 cfg → state.ncst；runtime `nc_state_set_*` 只改 RAM（見 §3）。
<!-- END GENERATED: schema -->

**enum 的寫法**：config JSON 寫名字（例 `LED.backend = "i80"`），`state.ncst` 存 index
（`i80` → 1）；schema 的 `values[]` 就是這個對照，`nc_state_set_u32()` 寫 index。

### 1.1 怎樣加／減一個 key

```bash
# 1) 改 schema/state-keys.json（id = domain|unit|index|field，各 8 bit；填 type／min／max／enum）
# 2) 生成 C 表＋本表
python3 tools/gen_state_keys.py --doc
# 3) 如果開機要寫入：nc_state_boot.c 的 build_records() 加一行由 cfg 取值
# 4) rebuild（改過 sdkconfig.defaults 要 --fresh）
```

- 生成器會驗：id 的 unit byte 同 unit id 一致、id 唔可以係 0、id／name 唔重複、範圍在型別內、enum 由 0 連續。
- 刪 key 之後，舊 `state.ncst` 的未知 key 會自動跳過（向前兼容），唔使清檔。
- `status cfg` 的 `schema.bad` 會發現「code 加咗 key、schema 未加」的 drift。

---

## 2. 三種「有冇入 schema」的狀態（2026-11-27）

| 狀態 | 意思 | 邊啲欄位 | `nc_state_set_*`？ |
|---|---|---|---|
| **入 schema・`value`** | 開機由 cfg 造 record 入 store；可回寫；`apply_to_cfg()` 後消費者讀 cfg | `system.json` 4（cID／frame_interval_ms／num_pixels／log_debug_mode）、`hardware.json` 12（I80 5＋SPI 7）、`driver.json` 10（LED）＝**26 個** | ✅（只改 RAM；永久改仍要改 JSON） |
| **入 schema・`struct`** | 只登記 path／型別／範圍；由 cfg 直接消費，**唔入 store**（唔佔 RAM） | `system.json` 72（links 64＋routes 8）、`network.json` 3（wifi）、`hardware.json` 185（RMT／PCNT／ADC／TWAI／MCPWM／PARLIO／LCD／touch／cam…未有 driver 消費） | ⚠ 唔喺 store；現時 set 意義有限 |
| **未入 schema** | `nc_cfg_load.c` 有 parse，但 registry 冇呢個 key | `tasks.json`（level／affinity／prio，**刻意移除**：任務熱插拔，改走動態 key `0xF0` 域＋name hash）、`resources.json`（provide／need）、`hardware.json` 未 typed 段（I2C／I2S／ENC／XL9555）、`driver.json` 其他 device（TFT／PCA9685／SDcard…） | ❌ 改 JSON＋reboot |

- **判斷法則**：`schema/state-keys.json` 搵到 → 有「名分」；再睇 `class`：
  `value` 會入 store／blob，`struct` 只登記。
- **tasks／effects 例外**：會熱插拔（`.nct`／`.nce`），字典靜態化一定失效；
  用 `key = 0xF0<<24 | FNV-1a(name)`，由管理器提供身份（見
  [`state-dynamic-keys.md`](../03_notes/state-dynamic-keys.md)）。
- **pixel 內容**（effects／map／mode／registry）唔入 config／schema，係自己的
  `NCLAYOUT` 快取；driver 只決定「燈幾何」，pixel JSON 決定「播咩」。
- 逐 key 的實際範圍／enum 以 §1 生成表為準；**driver 的 parser 實際範圍另見
  [`driver-config.md`](driver-config.md) §4**（schema 同 parser 未完全對齊）。

---

## 3. 三種設定路徑（選錯層 = 改完唔生效）

| 路徑 | 做法 | 幾時生效 | 適合 |
|---|---|---|---|
| **FAT live**（最高權威） | 用 USB MSC／WebMaster／`nc4_file.py` 改裝置 `/data/config/*.json` | **reboot** | 現場調參、唔想重燒 |
| **build 預置** | 改 `ports/project/<chip>/<board>/data/config/*.json` 或 `sdkconfig.defaults` | **rebuild（`--fresh`）→ OTA**；裝置已有舊檔時要 `fs_format` 才會重新 seed | 出廠預設、換 project |
| **runtime API**（RAM） | `nc_state_set_u32/i32/bool()`（只存數值；`set_str` 已退場）；熱路徑用 `nc_state_bind_*()` 拿定位址 | **即時**（只改 RAM＋`dirty`）；`STATUS_SET {"state":…}` 排隊，reboot 前由 main 落盤；reboot 由 JSON 重算 | 臨時調參、測試；永久改仍然要改 JSON |

- **build 預置只會在裝置缺檔時 seed**；檔案存在就唔會被覆蓋（`fs_format=1` 才會清 `/data` 重 seed）。
- **改 `sdkconfig.defaults` 一定要 `--fresh`**，否則 build dir 會沿用舊 `sdkconfig`
  （2026-11-26 實際撞過：main stack 仍然 3,072 → 爆棧、USB 唔 enumerate、燈全黑）。

---

## 4. 改完之後的驗證清單

```bash
# 1) config 有冇 parse 問題、state 有冇 hit、schema drift
python3 tools/nc4.py --port <PORT> status cfg
#    state : {"hit":1,"cmp_ok":1,"rebuilt":0,"n":4,"bytes":50,"store":4,"apply":4,"inactive":22}
#    act   : {"src":...,"en":4,"units":"usb,i80,led","fat":...,"match":1}
#    schema: {"ver":2,"crc":...,"keys":286,"units":16,"bad":0,"dirty":0}
#    （數字示例＝SuperMini／HEAD；舊 firmware 0.1.2 冇 state／schema 欄位，先重建 OTA）

# 2) 任務開咗未（四層四任務要 level=4 started=1 running=1）
python3 tools/nc4.py --port <PORT> status tasks

# 3) 通道開咗未（links[].enable=0 就唔會出現）
python3 tools/nc4.py --port <PORT> status net

# 4) 換 project／改壞 config：清 /data 重 seed
python3 tools/nc4_file.py --port <PORT> set '{"System":{"fs_format":1}}' 1
python3 tools/nc4.py --port <PORT> reboot
```

| 欄位 | 正常 | 唔正常代表 |
|---|---|---|
| `cfg.*.miss/bad/inv/unk` | 0（或缺檔時 miss>0） | `bad`＝壞檔、`inv`＝超範圍、`unk`＝未知欄位 |
| `state.hit` | 1 | 0 ＝ config 改過（值有變會重建，`rebuilt=1`） |
| `state.rebuilt` | 0 | 1 ＝ 今次寫過盤（值有變／壞檔） |
| `act.match` | 1 | 0 ＝ FAT 的 hardware／driver 同 build 預置唔同（activation 係舊 build） |
| `schema.bad` | 0 | >0 ＝ code 造出的 record 同 schema 型別／範圍唔一致（要修） |

---

## 5. 與圖的對應

| 圖 | 檔案 | 睇咩 |
|---|---|---|
| 設定流轉／權威層 | [config-layers.svg](../00_pm/config-layers.svg) | 三來源 → B02 → cfg → 消費者；state 快取角色；三種改法 |
| schema registry | [schema-registry.svg](../00_pm/schema-registry.svg) | 一個 JSON → 三份生成檔 → 查表／API；16 個 unit 的 key 與範圍 |
| 動畫解釋（19 步） | [config-state-flow.html](../00_pm/config-state-flow.html) | config→state→pixel JSON→緩衝；含「6 個 config 檔邊個有遮罩」 |
