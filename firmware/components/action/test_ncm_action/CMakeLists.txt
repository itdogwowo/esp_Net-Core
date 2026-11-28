# ncm 的開機接線（測試能力；由 main.c 搬出）。
#
# 名字帶 test_ 就係登記：`firmware/CMakeLists.txt` 的 glob 只喺 NC_ENABLE_TESTS
# 收 components/*/test_*，所以 release 完全唔含呢個 component。
idf_component_register(
    SRCS "nc_ncm_boot.c"
    INCLUDE_DIRS "."
    REQUIRES nc4 task_manager mem ncm plugin ota_action esp_timer freertos log
)
