# pixel_encode_task（tasks 層）：pixel 管線第三層 —— 編碼。
#
# 把環裡的 4 B/px（R,G,B,W）編成「該硬件的線材格式」：
#   · I80／SPI：bus word／bit stream
#   · C 系列（C3/C6）／S3：RMT symbols（見 drv_rmt）
#   · P4：PARLIO 並行 bus word（見 drv_parlio）
# 所以叫 encode，唔叫 wave（wave 只涵蓋 WS2812 波形）。
#
# 只擁有 loop／節拍／生命週期；環、lane、後端、定時器由 render_task（公用輸出層）
# 擁有。編碼經 nc_render_encode_ready／wait／tick 與 render 合作；兩者只傳
# 「格索引」隊列，唔 copy 像素。
idf_component_register(
    SRCS "nc_px_encode_task.c"
    REQUIRES task core render_task
    PRIV_REQUIRES freertos
    # 任務由 constructor 自己登記；冇 symbol 被 main 引用，必須 WHOLE_ARCHIVE。
    WHOLE_ARCHIVE
)
