# 有 USB OTG 的晶片才需要 tinyusb（MSC 的 LUN 掛載 API 都在那裡）。
# 用 IDF_TARGET 而不是 CONFIG_NETCORE_USB_OTG：第一次 configure 時 CONFIG_*
# 還未生成（同 main/CMakeLists.txt 的理由）。
set(_store_reqs wear_levelling fatfs esp_partition)
if(IDF_TARGET STREQUAL "esp32s3" OR IDF_TARGET STREQUAL "esp32s2" OR IDF_TARGET STREQUAL "esp32p4")
    list(APPEND _store_reqs espressif__esp_tinyusb)
endif()

idf_component_register(
    SRCS "nc_store.c"
    INCLUDE_DIRS "include"
    REQUIRES ${_store_reqs}
)
