# modules/actions/ — 指令模組（外掛私有指令）

> 放「外掛自己嘅指令」模組。leaf 名要 **以 `_actions` 結尾**（例如 `test_echo_actions/`）。

規則：

- 編譯輸出 `.nct`（容器 kind=normal），**但 `build.json` 必須宣告 `"cmds": [0x3xxx]`**
  （外掛保留區 0x3000–0x3FFF；核心區唔可以由外掛佔用）。
- 指令 handler 唔係由檔案載入：由 ncm 開機時註冊入 action 表，收到指令後經
  mediator 呼叫模組 export；**核心／OTA／file 指令永遠留固件**（見
  [commands.md §1.1–1.2](../../docs/01_protocol/commands.md)）。
- 未實作路由之前，呢個目錄係約定保留位。

資料夾結構同 `tasks/` 一樣：`<name>_actions/`＋`<name>_actions.wat`＋`build.json`＋`meta.json`。
