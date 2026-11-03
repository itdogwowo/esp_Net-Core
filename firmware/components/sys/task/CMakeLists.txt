# 任務的「型別 + 執行層」：nc_task_t 的定義、loop 的建立/量測/停止。
# 對照舊專案 lib/sys/task.py（Task 基底），管理器的職責在 ../task_manager。
idf_component_register(
    SRCS "nc_task_loop.c"
    INCLUDE_DIRS "include" "."
    REQUIRES freertos mem esp_timer
)
