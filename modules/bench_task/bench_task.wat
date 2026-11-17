;; 外掛版的 CPU 基準：同 0xF050 CPU_BURN（native C）一模一樣的 LCG 迴圈。
;; 目的：同一塊板、同一個 workload，量「wasm 直譯 vs native C」嘅實際差距。
;; 做法：task_tick 用 nc_now_us() 量自己嘅執行時間，print 一行。
(module
  (import "env" "nc_print"     (func $print    (param i32 i32) (result i32)))
  (import "env" "nc_print_i32" (func $print_i32 (param i32)    (result i32)))
  (import "env" "nc_now_us"    (func $now      (result i64)))

  (memory (export "memory") 1)
  (data (i32.const 0)  "bench iters=50000 us=")   ;; 21 B
  (data (i32.const 32) " sum=")                    ;; 5 B

  (func (export "task_tick") (result i32)
    (local $i i32)
    (local $v i32)
    (local $t0 i64)

    (local.set $t0 (call $now))
    (local.set $v (i32.const 1))
    (block $done
      (loop $lp
        (br_if $done (i32.ge_u (local.get $i) (i32.const 50000)))
        (local.set $v
          (i32.add (i32.mul (local.get $v) (i32.const 1664525))
                   (i32.const 1013904223)))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $lp)))

    (drop (call $print (i32.const 0) (i32.const 21)))
    (drop (call $print_i32 (i32.wrap_i64 (i64.sub (call $now) (local.get $t0)))))
    (drop (call $print (i32.const 32) (i32.const 5)))
    (drop (call $print_i32 (local.get $v)))
    (i32.const 0))
)
