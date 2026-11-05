# 開機自測（測試任務）；只喺 NC_ENABLE_TESTS=ON 時編譯。
idf_component_register(
    SRCS "nc_probe.c"
    INCLUDE_DIRS "include"
    REQUIRES task mem
)
