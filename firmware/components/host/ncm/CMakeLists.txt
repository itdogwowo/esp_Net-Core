idf_component_register(
    # 模組載入器（政策）：掃描／arena／live／預算／attach。
    # 「怎麼跑」唔喺這裡 —— 係 sw/plugin（nc_plugin_*）。
    # 2026-11-17 由 components/tasks/test_mod_tasks 搬入 components/ncm；
    # 2026-11-28 拆出插件工具，解碼／引擎搬去 components/sw/plugin。
    SRCS "nc_mod.c" "nc_res.c"
    INCLUDE_DIRS "include"
    REQUIRES mem esp_rom freertos esp_timer task_manager config plugin
)
