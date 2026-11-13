# 任務管理器：登記 → 宣告資源 → 啟動 → 開 loop → 狀態。
# 對照舊專案 lib/sys/task_manager.py；任務型別與 loop 執行在 ../task。
#
# 注意：本元件**沒有自己的公開標頭**——`nc_task.h` 屬於 ../task（型別與 loop 執行都在那邊）。
# 所以這裡不宣告 INCLUDE_DIRS；硬寫 "include" 會令 CMake 在 configure 階段直接失敗
# （idf_component_register 會檢查目錄是否存在）。
idf_component_register(
    SRCS "nc_task.c"
    REQUIRES freertos mem esp_timer config task core
)
