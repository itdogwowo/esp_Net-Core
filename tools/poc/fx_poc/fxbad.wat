;; fxbad.wat — 壞掉的效果模組：fx_init 直接回 1（拒絕載入）。
;;
;; 用途：證明「上傳壞檔案 = 被拒絕，舊效果繼續跑」，不是變磚。
(module
  (memory (export "memory") 1)

  (func (export "fx_abi_version") (result i32)
    (i32.const 0))

  (func (export "fx_init") (param $d i32) (result i32)
    (i32.const 1))          ;; 1 = 拒絕

  (func (export "fx_render")
        (param $fid i32) (param $t i64) (param $off i32) (param $len i32))
)
