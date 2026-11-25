# pixel_paint_task（tasks 層）：抖動＋配對＋上色。
#
# 由 CONFIG_NETCORE_CONTENT 開關（Kconfig：depends NETCORE_PIXEL，default n）。
# 關閉時 nc_px_paint_task.c 整段 #if 掉 → component 只是一個空 object，唔登記任務／指令。
#
# 職責：on_start 載 /data/pixel 四層 → 建 plan；每幀由 effect 拿最新值流 →
# 按 ranges/write 配對（抖動在內圈）→ white mix → 寫 prd 環；0xF06B/0xF06D/0xF06F。
#
# 同 effect 的介面在 pixel_effect_task 的 include/nc_pipeline.h：
# paint publish aprog、effect snapshot；effect copy frame、paint ack。
idf_component_register(
    SRCS "nc_px_paint_task.c"
    REQUIRES nc4 pixel task store pixel_effect_task render_task
    PRIV_REQUIRES freertos esp_timer heap log
    WHOLE_ARCHIVE
)
