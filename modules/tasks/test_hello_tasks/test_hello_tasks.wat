;; 最小外部任務模組（.nct）—— 驗證 ncm 載入 → 實例化 → 定時 tick 全鏈。
;;
;; 做的事：
;;   task_init：印一行「ext-task (wasm) init」
;;   task_tick：每 tick 一行 “tick pin21 read=<值> toggle=<新值>”
;;              → 讀 GPIO21、反轉 GPIO21（1 Hz，見 build.sh 的 --fps 1）
;;
;; host imports（白名單，見 nc_mod_abi.h）：nc_print / nc_print2 /
;; nc_gpio_read / nc_gpio_toggle。沒有 pointer、沒有 syscall，
;; 唯一出入口就係這幾條 import。
(module
  (import "env" "nc_print"       (func $print      (param i32 i32) (result i32)))
  (import "env" "nc_print2"      (func $print2     (param i32 i32 i32 i32 i32 i32) (result i32)))
  (import "env" "nc_gpio_read"   (func $gpio_read  (param i32)     (result i32)))
  (import "env" "nc_gpio_toggle" (func $gpio_toggle (param i32)    (result i32)))

  ;; 1 page（64 KB）linear memory：只放下面幾段字串。
  (memory (export "memory") 1)
  (data (i32.const 0)  "ext-task (wasm) init")   ;; 20 B
  (data (i32.const 32) "tick pin21 read=")       ;; 16 B
  (data (i32.const 64) " toggle=")               ;; 8 B

  ;; 啟動時一次
  (func (export "task_init") (result i32)
    (drop (call $print (i32.const 0) (i32.const 20)))
    (i32.const 0))

  ;; 每個 tick（manifest fps = 1 → 1 秒一次）
  ;; nc_print2 -> 一行過： "<文字1><v1> <文字2><v2>"
  (func (export "task_tick") (result i32)
    (drop (call $print2
      (i32.const 32) (i32.const 16) (call $gpio_read (i32.const 21))
      (i32.const 64) (i32.const 8)  (call $gpio_toggle (i32.const 21))))
    (i32.const 0))
)
