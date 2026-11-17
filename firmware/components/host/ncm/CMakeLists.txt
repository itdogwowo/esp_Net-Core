idf_component_register(
    # 外部任務宿主（前身 test_mod_tasks）：掃描／容器解析／資源表／WAMR 引擎。
    # 喺 NC_ENABLE_NCM 建置才會加入（測試映像自動開；release 預設冇）。
    # 2026-11-17 由 components/tasks/test_mod_tasks 搬入 components/ncm，
    # 準備将来升 production（唔使改名再搬一次）。
    SRCS "nc_mod.c" "nc_ncd.c" "nc_res.c" "nc_meta.c" "nc_mod_engine.c"
    INCLUDE_DIRS "include"
    REQUIRES mem esp_rom freertos esp_timer task_manager config ota_action
             espressif__wasm-micro-runtime esp_driver_gpio
)
