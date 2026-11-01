# fx_poc — 效果模組 ABI 的 host 參考實作

在 Mac 上驗證「host ↔ 燈效模組」的呼叫約定，**不需要板子**。設計理由與選型見
[docs/03_notes/effect-language.md](../../../docs/03_notes/effect-language.md)。

```bash
node tools/poc/fx_poc/host.mjs tools/poc/fx_poc/fx.wasm --frames 120 --ppm /tmp/frame.ppm
```

## 三個情境（就是「分離式固件」的三個必要條件）

```bash
# ① 跑一個效果
node tools/poc/fx_poc/host.mjs tools/poc/fx_poc/fx.wasm --frames 120

# ② 第 120 幀換成另一份檔案（不重啟、不重配記憶體、不重算佈局）
node tools/poc/fx_poc/host.mjs tools/poc/fx_poc/fx.wasm --frames 240 \
     --swap tools/poc/fx_poc/fx2.wasm --at 120

# ③ 換成壞檔案 → 被拒絕，舊的繼續跑（不變磚）
node tools/poc/fx_poc/host.mjs tools/poc/fx_poc/fx.wasm --frames 240 \
     --bad tools/poc/fx_poc/fxbad.wasm --at 120
```

![換檔前後](frame.png)

## 檔案

| 檔案 | 角色 |
|---|---|
| `fx.wat` / `fx.wasm` | **測試替身 A**：徑向脈衝（367 B）。不是給人寫燈效用的，只是讓 harness 有東西可載入 |
| `fx2.wat` / `fx2.wasm` | **測試替身 B**：斜向掃描（338 B）。ABI 相同、算式不同，用來證明「換檔案＝換效果」 |
| `fxbad.wat` / `fxbad.wasm` | **壞模組**：`fx_init` 回 1，用來證明壞檔不會把系統帶走 |
| `host.mjs` | host 端參考實作：算佈局 → 寫 descriptor → `fx_init` → 每幀 `fx_render` → 讀回幀緩衝 |
| `frame.png` | 範例輸出（左 A、右 B，26×26 放大 12 倍） |

重編 `.wasm`（只有改了 `.wat` 才需要）：

```bash
npx wat2wasm tools/poc/fx_poc/fx.wat   -o tools/poc/fx_poc/fx.wasm
npx wat2wasm tools/poc/fx_poc/fx2.wat  -o tools/poc/fx_poc/fx2.wasm
npx wat2wasm tools/poc/fx_poc/fxbad.wat -o tools/poc/fx_poc/fxbad.wasm
```

## ABI v0

```wat
fx_abi_version() -> i32                                   ; 必須回 0
fx_init(desc_ptr: i32) -> i32                             ; 0 = OK；非 0 = 拒絕
fx_render(frame_id: i32, time_us: i64, span_off: i32, span_len: i32)
```

descriptor（host 寫進 guest 線性記憶體，全部 little-endian u32）：

```
+0 px_count  +4 width   +8 height
+12 fb_off   +16 fb_bytes
+20 sc_off   +24 sc_bytes
```

幀緩衝是 canonical `px × 4 B（R,G,B,W）`，直接住在 guest 的線性記憶體裡 —— 每像素 0 次 host 呼叫。

## 為什麼要有這個

1. **先驗約定，再驗硬體**。WAMR 在 P4 上能不能跑是 P0 spike 的事；ABI 對不對是今天就能知道的事。
2. **conformance test**。效果只用整數運算時，host 與板子應該得到**同一個 checksum**（`host.mjs` 會印）。這是「ABI 接錯」最便宜的防線。
3. **換檔的語義先定好**：成功才 swap、失敗保留舊的、記憶體不重配。這三件事在 host 上一天就能驗完，不用等板子。
4. 等 TinyGo 工具鏈好了，把 `fx.wasm` 換成 TinyGo 產的 `.wasm` 即可，`host.mjs` 不用改。
