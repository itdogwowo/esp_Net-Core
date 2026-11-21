# pipeline_task（tasks 層）：三任務管線的 pair＋effect。
#
# 由 CONFIG_NETCORE_CONTENT 開關（Kconfig：depends NETCORE_PIXEL，default n）。
# 關閉時 nc_pair_task.c 內容整段 #if 掉 → component 只是一個空 object，
# 唔登記任務、唔登記指令（同 effect_bench 同一個模式）。
#
# 職責：
#   pair  ：on_start 載四層→建 plan；讀 effect fixed frame→按 ranges/write 配對→
#           white mix→prd 環；0xF06B/0xF06D/0xF06F 指令。
#   effect：只 evaluate program 波形值（50 fps），經 nc_pipeline.h 的 fixed frame
#           交給 pair；唔碰 map／write／ring。
#
# 送燈仍然係 render 任務（tasks/render_task）的事；兩者只透過環見面。
idf_component_register(
    SRCS "nc_pair_task.c" "nc_effect_task.c" "nc_pipe_stats.c"
    # pixel：環／layer 掃描／show plan／合成。store：/data 掛載點（nc_store_fs_base）。
    # task：任務管理器。nc4：指令表＋ decode context。
    REQUIRES nc4 pixel task store render_task
    INCLUDE_DIRS "include"
    PRIV_REQUIRES freertos esp_timer heap log
    # 指令表同任務都係 constructor 自己登記；冇任何 symbol 被 main 引用，
    # 唔加 WHOLE_ARCHIVE 就會整份 object 被 linker 丟掉。
    WHOLE_ARCHIVE
)
