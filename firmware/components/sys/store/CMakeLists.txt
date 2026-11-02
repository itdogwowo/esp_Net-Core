idf_component_register(
    SRCS "nc_store.c"
    INCLUDE_DIRS "include"
    REQUIRES espressif__esp_tinyusb wear_levelling fatfs esp_partition
)
