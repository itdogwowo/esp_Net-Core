;; fx2.wat — 第二個測試替身：斜向掃描。用來證明「換一份檔案就換一個效果」，
;; 不需要重燒、不需要重開機，descriptor 與記憶體位址完全不變。
;;
;; ABI 與 fx.wat 完全相同（v0），只有每像素的算式不同。
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
    (local $q i32) (local $u i32) (local $v i32) (local $o i32) (local $phase i32)

    (i32.store offset=0 (global.get $sc)
      (i32.add (i32.load offset=0 (global.get $sc)) (i32.const 1)))

    (local.set $i (local.get $off))
    (local.set $end (i32.add (local.get $off) (local.get $len)))
    (local.set $phase (i32.wrap_i64 (i64.shr_u (local.get $t) (i64.const 5))))

    (block $done
      (loop $l
        (br_if $done (i32.ge_u (local.get $i) (local.get $end)))

        ;; 斜向條紋：相位 = (x + y) * 8 + 時間/32
        (local.set $x (i32.rem_u (local.get $i) (global.get $w)))
        (local.set $y (i32.div_u (local.get $i) (global.get $w)))
        (local.set $q (i32.add
                        (i32.shl (i32.add (local.get $x) (local.get $y)) (i32.const 3))
                        (local.get $phase)))
        (local.set $u (i32.and (local.get $q) (i32.const 255)))
        (local.set $v (select (i32.shl (local.get $u) (i32.const 1))
                              (i32.shl (i32.sub (i32.const 255) (local.get $u))
                                       (i32.const 1))
                              (i32.lt_u (local.get $u) (i32.const 128))))

        (local.set $o (i32.add (global.get $fb)
                               (i32.shl (local.get $i) (i32.const 2))))
        (i32.store8 offset=0 (local.get $o) (i32.sub (i32.const 255) (local.get $v)))
        (i32.store8 offset=1 (local.get $o) (local.get $v))
        (i32.store8 offset=2 (local.get $o) (i32.and (local.get $q) (i32.const 255)))
        (i32.store8 offset=3 (local.get $o) (i32.const 255))   ;; W 開滿，跟 fx 區別

        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $l))))
)
