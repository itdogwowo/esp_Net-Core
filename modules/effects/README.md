# modules/effects/ — 效果模組

> 放燈效／畫面效果模組。leaf 名要 **以 `_effects` 結尾**（例如 `pixel_effects/`）。

- 編譯輸出 `.nce`（容器 kind=effect，需要 `--px`）。
- 每 tick render 到 framebuffer；driver（RMT／LCD）留固件（L2/L3）。
- 資料夾結構同 `tasks/` 一樣：`<name>_effects/`＋`<name>_effects.wat`＋`build.json`＋`meta.json`。
