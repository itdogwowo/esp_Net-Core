# show_task（tasks 層）：production 的「檔案驅動 show」生產者。
#
# 由 CONFIG_NETCORE_SHOW 開關（Kconfig：depends NETCORE_PIXEL，default n）。
# 關閉時 nc_show_task.c 內容整段 #if 掉 → component 只是一個空 object，
# 唔登記任務、唔登記指令（同 effect_bench 同一個模式）。
#
# 職責只有三件：
#   ① 開機（on_start）：nc_px_layer_load_all(<fs_base>/pixel) → 揀 mode／map →
#      nc_px_show_plan() 建 plan（mode 四層資料的解釋在 sw/pixel，唔在這裡）；
#   ② 每幀：清 canvas → nc_px_show_frame(plan, step) → white mix → 寫 prd 環；
#   ③ 0xF06B SHOWFRAME（回 0xF06C）：把最近一幀 4 B/px 以 hex 交主機（Web UI poll）。
#
# 送燈仍然係 render 任務（tasks/render_task）的事；兩者只透過環見面。
idf_component_register(
    SRCS "nc_show_task.c"
    # pixel：環／layer 掃描／show plan／合成。store：/data 掛載點（nc_store_fs_base）。
    # task：任務管理器。nc4：指令表＋ decode context。
    REQUIRES nc4 pixel task store
    PRIV_REQUIRES freertos esp_timer heap log
    # 指令表同任務都係 constructor 自己登記；冇任何 symbol 被 main 引用，
    # 唔加 WHOLE_ARCHIVE 就會整份 object 被 linker 丟掉。
    WHOLE_ARCHIVE
)
