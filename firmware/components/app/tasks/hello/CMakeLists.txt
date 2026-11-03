# hello：最小框架自測任務（每秒印一次）。
# 每個任務一個 component，對照舊專案 slave/tasks/<name>.py。
idf_component_register(
    SRCS "nc_hello.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos esp_timer task
)
