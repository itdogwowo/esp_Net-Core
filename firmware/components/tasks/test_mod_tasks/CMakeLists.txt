idf_component_register(
    # nc_mod_engine.c = WAMR backend（外部任務／效果模組的執行引擎）。
    # 只存在於 NC_ENABLE_TESTS 的建置（整個 component 都係 tests-only）。
    SRCS "nc_mod.c" "nc_ncd.c" "nc_res.c" "nc_meta.c" "nc_mod_engine.c"
    INCLUDE_DIRS "include"
    REQUIRES mem esp_rom freertos esp_timer task_manager config ota_action
             espressif__wasm-micro-runtime esp_driver_gpio
)
