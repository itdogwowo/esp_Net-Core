# modules/effects/ — 效果模組（USER 平面）

> 放燈效／畫面效果模組。leaf 名要 **以 `_effects` 結尾**（例如 `fire_effects/`）。
>
> 呢度只放**載入式**效果（`.nce`）。另外兩個交付：**CORE 內建**效果住
> `firmware/components/effects/<域>_effects/`；**DATA** 的畫波 program 住
> `/data/pixel/effects/*.json`。三者同一個 canonical frame 契約，差別只在交付方式。
> 見 [behavior-taxonomy.md §3](../../docs/03_notes/behavior-taxonomy.md)。

- 編譯輸出 `.nce`（容器 kind=effect，需要 `--px`）。
- 每 tick render 到 framebuffer；driver（RMT／LCD）留固件（L2/L3）。
- 資料夾結構同 `tasks/` 一樣：`<name>_effects/`＋`<name>_effects.wat`＋`build.json`＋`meta.json`。
