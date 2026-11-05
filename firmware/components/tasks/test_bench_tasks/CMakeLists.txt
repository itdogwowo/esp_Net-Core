# 基準任務：只為「大循環」量測的驗證存在。預設唔跑（tasks.json 補寫成 [0,0]）。
idf_component_register(
    SRCS "nc_bench.c"
    INCLUDE_DIRS "include"
    REQUIRES task esp_hw_support
)
