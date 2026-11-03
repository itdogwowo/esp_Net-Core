# 任務管理器：登記 → 宣告資源 → 啟動 → 開 loop → 量測 → 狀態。
# 對照舊專案 lib/sys/task_manager.py；任務型別與 loop 執行在 ../task。
idf_component_register(
    SRCS "nc_task.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos mem esp_timer config task
)
