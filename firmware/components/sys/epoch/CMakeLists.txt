# 每核「大循環」量測：純邏輯（唔靠 IDF / FreeRTOS），所以 host test 可以直接編。
idf_component_register(
    SRCS "nc_epoch.c"
    INCLUDE_DIRS "include"
)
