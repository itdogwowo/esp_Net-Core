# 插件工具（解碼＋運行）：認得 .nct／.nce、實例化、每 tick 跑、收工。
# 只有 NC_ENABLE_TESTS 建置才會加入（同 host/ncm 一齊）；release 唔編、亦唔 fetch WAMR。
#
# 這裡係唯一識 WAMR 的地方：換引擎（AOT／其他 runtime）只換 nc_plugin_wamr.c。
idf_component_register(
    SRCS "nc_plugin.c" "nc_plugin_wamr.c" "nc_ncd.c" "nc_meta.c"
    INCLUDE_DIRS "include"
    REQUIRES log heap esp_timer esp_driver_gpio soc espressif__wasm-micro-runtime
)
