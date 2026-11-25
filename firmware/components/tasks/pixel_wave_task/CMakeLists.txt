# pixel_wave_task（tasks 層）：pixel 管線第三層 —— 畫波。
#
# 只擁有 loop／節拍／生命週期；環、lane、後端、定時器由 render_task（公用輸出層）
# 擁有。畫波經 nc_render_wave_ready／wait／tick 與 render 合作；兩者只傳
# 「格索引」隊列，唔 copy 像素。
idf_component_register(
    SRCS "nc_px_wave_task.c"
    REQUIRES task core render_task
    PRIV_REQUIRES freertos
    # 任務由 constructor 自己登記；冇 symbol 被 main 引用，必須 WHOLE_ARCHIVE。
    WHOLE_ARCHIVE
)
