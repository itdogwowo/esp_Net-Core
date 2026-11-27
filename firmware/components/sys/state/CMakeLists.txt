# 狀態管理器：NCSTATE 格式、FAT 持久、開機 sync、schema registry、讀寫 API。
# schema/state-keys.json 係唯一名單；本 component 生成並使用它（見 nc_state_schema.h）。
idf_component_register(
    SRCS "nc_state.c" "nc_boot.c" "nc_state_blob.c" "nc_state_file.c" "nc_state_boot.c" "nc_state_commit.c" "nc_state_store.c" "nc_state_api.c" "nc_state_schema.c" "nc_state_schema_data.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash esp_app_format config
)
