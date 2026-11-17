;; API 呼叫成本量測（N=2000）：每次 tick 用 print2 印兩行，避免逐個 print 被拆行。
;;   line1: empty_us=<e> now_us=<n>     line2: gpio_us=<g> lcg_us=<l>
;; per-call 成本 = (now/gpio - empty) / N。
(module
  (import "env" "nc_print2"    (func $print2 (param i32 i32 i32 i32 i32 i32) (result i32)))
  (import "env" "nc_now_us"    (func $now    (result i64)))
  (import "env" "nc_gpio_read" (func $gpio   (param i32) (result i32)))

  (memory (export "memory") 1)
  (data (i32.const 0)  "empty_us=")   ;; 9 B
  (data (i32.const 16) "now_us=")     ;; 7 B
  (data (i32.const 32) "gpio_us=")    ;; 8 B
  (data (i32.const 48) "lcg_us=")     ;; 7 B
  (global $N i32 (i32.const 2000))

  (func $empty (param $n i32) (result i64)
    (local $i i32) (local $t0 i64)
    (local.set $t0 (call $now))
    (block $done
      (loop $lp
        (br_if $done (i32.ge_u (local.get $i) (local.get $n)))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $lp)))
    (i64.sub (call $now) (local.get $t0)))

  (func $now_calls (param $n i32) (result i64)
    (local $i i32) (local $t0 i64)
    (local.set $t0 (call $now))
    (block $done
      (loop $lp
        (br_if $done (i32.ge_u (local.get $i) (local.get $n)))
        (drop (call $now))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $lp)))
    (i64.sub (call $now) (local.get $t0)))

  (func $gpio_calls (param $n i32) (result i64)
    (local $i i32) (local $t0 i64) (local $acc i32)
    (local.set $t0 (call $now))
    (block $done
      (loop $lp
        (br_if $done (i32.ge_u (local.get $i) (local.get $n)))
        (local.set $acc (i32.add (local.get $acc) (call $gpio (i32.const 21))))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $lp)))
    (i64.sub (call $now) (local.get $t0)))

  (func $lcg (param $n i32) (result i64)
    (local $i i32) (local $v i32) (local $t0 i64)
    (local.set $t0 (call $now))
    (local.set $v (i32.const 1))
    (block $done
      (loop $lp
        (br_if $done (i32.ge_u (local.get $i) (local.get $n)))
        (local.set $v
          (i32.add (i32.mul (local.get $v) (i32.const 1664525))
                   (i32.const 1013904223)))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $lp)))
    (i64.sub (call $now) (local.get $t0)))

  (func (export "task_tick") (result i32)
    (drop (call $print2 (i32.const 0) (i32.const 9)
                        (i32.wrap_i64 (call $empty (global.get $N)))
                        (i32.const 16) (i32.const 7)
                        (i32.wrap_i64 (call $now_calls (global.get $N)))))
    (drop (call $print2 (i32.const 32) (i32.const 8)
                        (i32.wrap_i64 (call $gpio_calls (global.get $N)))
                        (i32.const 48) (i32.const 7)
                        (i32.wrap_i64 (call $lcg (global.get $N)))))
    (i32.const 0))
)
