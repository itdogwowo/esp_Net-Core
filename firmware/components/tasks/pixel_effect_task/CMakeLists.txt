# pixel_effect_task（tasks 層）：管線第一段 —— 只生產波形值。
#
# 由 CONFIG_NETCORE_CONTENT 開關（Kconfig：depends NETCORE_PIXEL，default n）。
# 關閉時 nc_px_effect_task.c 內容整段 #if 掉 → component 只是空 object，
# 唔登記任務、唔佔 buffer（同 effect_bench 同一個模式）。
#
# 職責：按 plan 的 aprog 逐條 evaluate program → 雙 pool（ack 防撕裂）→
# 經 nc_pipeline.h 的 fixed frame 交指標給 pixel_paint_task。唔碰 map／write／環。
idf_component_register(
    SRCS "nc_px_effect_task.c" "nc_px_pipe_stats.c"
    REQUIRES task pixel
    INCLUDE_DIRS "include"      # nc_pipeline.h：effect ↔ pair 的唯一介面
    PRIV_REQUIRES freertos esp_timer heap log
    # 任務同 pipe API 都係 constructor 自己登記；冇 symbol 被 main 直接引用，
    # 唔加 WHOLE_ARCHIVE 就會整份 object 被 linker 丟掉。
    WHOLE_ARCHIVE
)
