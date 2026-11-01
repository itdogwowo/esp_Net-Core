;; fx.wat — 測試替身（test double），不是給人寫燈效的語言。
;;
;; 用途：在 Mac 上驗證 host ↔ 效果模組的 ABI（tools/fx_poc/host.mjs 會載入它）。
;; 真正寫燈效時，這個 .wasm 由 TinyGo / Rust / C 產出，ABI 不變。
;;
;; ABI v0（全部 little-endian）：
;;
;;   desc +0   px_count      u32  像素數
;;   desc +4   width         u32  矩陣寬（1 = 條燈）
;;   desc +8   height        u32  矩陣高
;;   desc +12  fb_off        u32  幀緩衝在線性記憶體裡的 offset
;;   desc +16  fb_bytes      u32  = px_count * 4
;;   desc +20  sc_off        u32  第二塊 buffer（scratch）的 offset
;;   desc +24  sc_bytes      u32
;;
;; export：
;;   fx_abi_version() -> i32                     必須回 0
;;   fx_init(desc)    -> i32                     0 = OK；非 0 = 拒絕載入
;;   fx_render(frame_id, time_us, span_off, span_len)
;;       簽名對齊 architecture-flow.md 的 effects API：render(frame_id, time_us, span)。
;;       v0 的 span 一律是 [0, px_count)；參數先留著，之後要局部更新不用改 ABI。
(module
  (memory (export "memory") 1)

  (global $px (mut i32) (i32.const 0))
  (global $w  (mut i32) (i32.const 0))
  (global $h  (mut i32) (i32.const 0))
  (global $fb (mut i32) (i32.const 0))
  (global $sc (mut i32) (i32.const 0))

  (func (export "fx_abi_version") (result i32)
    (i32.const 0))

  (func (export "fx_init") (param $d i32) (result i32)
    (global.set $px (i32.load offset=0  (local.get $d)))
    (global.set $w  (i32.load offset=4  (local.get $d)))
    (global.set $h  (i32.load offset=8  (local.get $d)))
    (global.set $fb (i32.load offset=12 (local.get $d)))
    (global.set $sc (i32.load offset=20 (local.get $d)))
    (i32.store offset=0 (global.get $sc) (i32.const 0))
    (i32.const 0))

  (func (export "fx_render")
        (param $fid i32) (param $t i64) (param $off i32) (param $len i32)
    (local $i i32) (local $end i32) (local $x i32) (local $y i32)
    (local $dx i32) (local $dy i32) (local $d2 i32) (local $q i32)
    (local $u i32) (local $v i32) (local $o i32) (local $phase i32)

    ;; scratch[0] += 1：證明第二塊 buffer 真的拿得到、也真的寫得進去
    (i32.store offset=0 (global.get $sc)
      (i32.add (i32.load offset=0 (global.get $sc)) (i32.const 1)))

    (local.set $i (local.get $off))
    (local.set $end (i32.add (local.get $off) (local.get $len)))
    (local.set $phase (i32.wrap_i64 (i64.shr_u (local.get $t) (i64.const 3))))

    (block $done
      (loop $l
        (br_if $done (i32.ge_u (local.get $i) (local.get $end)))

        ;; 2D 座標 → 到中心的距離平方（整數，不用 float）
        (local.set $x  (i32.rem_u (local.get $i) (global.get $w)))
        (local.set $y  (i32.div_u (local.get $i) (global.get $w)))
        (local.set $dx (i32.sub (i32.shl (local.get $x) (i32.const 1)) (global.get $w)))
        (local.set $dy (i32.sub (i32.shl (local.get $y) (i32.const 1)) (global.get $h)))
        (local.set $d2 (i32.add (i32.mul (local.get $dx) (local.get $dx))
                                (i32.mul (local.get $dy) (local.get $dy))))

        ;; 徑向脈衝：相位 = 距離/4 + 時間/8，取三角波
        (local.set $q (i32.add (i32.shr_u (local.get $d2) (i32.const 2))
                               (local.get $phase)))
        (local.set $u (i32.and (local.get $q) (i32.const 255)))
        (local.set $v (select (i32.shl (local.get $u) (i32.const 1))
                              (i32.shl (i32.sub (i32.const 255) (local.get $u))
                                       (i32.const 1))
                              (i32.lt_u (local.get $u) (i32.const 128))))

        ;; 直接寫進 guest 自己的線性記憶體：每個像素 0 次 host 呼叫
        (local.set $o (i32.add (global.get $fb)
                               (i32.shl (local.get $i) (i32.const 2))))
        (i32.store8 offset=0 (local.get $o) (local.get $v))
        (i32.store8 offset=1 (local.get $o) (i32.sub (i32.const 255) (local.get $v)))
        (i32.store8 offset=2 (local.get $o) (i32.and (local.get $q) (i32.const 255)))
        (i32.store8 offset=3 (local.get $o) (i32.const 0))

        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $l))))
)
