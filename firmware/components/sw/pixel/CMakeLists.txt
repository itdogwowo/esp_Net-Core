# pixel（sw 層）：**工具**—— 環、編碼器、效果引擎（契約／registry／interpreter／抖動）。
#
# **純軟件**：不知道 ESP 硬體、不知道任務、不知道驅動。
# 引擎寫 4 B/px（R,G,B,W）進環；編碼器把 canonical 轉成各燈種的物理格式。
# 具體效果**唔**喺這裡：它們住 components/effects/<域>_effects/（實作）。
# 這一層的 .c 同時被 firmware 與 host 測試編譯（tools/hosttest/pixel_test、pxEngine_test、pxProgram_test）。
idf_component_register(
    SRCS "nc_pixel_q.c" "nc_pixel_encode.c" "nc_pixel_registry.c" "nc_pixel_bench.c"
         "engine/nc_px_engine_core.c" "engine/nc_px_engine_tables.c"
         "engine/nc_px_engine_program.c"
         "asset/nc_px_asset.c" "asset/nc_px_asset_scan.c" "asset/nc_px_json.c"
         "asset/nc_px_layer.c"
         "asset/nc_px_show.c"
         "asset/nc_px_layout.c"
         "asset/nc_px_layer_scan.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_timer log heap
)
