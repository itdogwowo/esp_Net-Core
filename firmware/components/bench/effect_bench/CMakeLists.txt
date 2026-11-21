# effect_bench：純效果管線效能（0xF065 EFBENCH，0xF066 回覆）
# ＋大緩衝 PSRAM 量測（0xF067 PSRAMBENCH，0xF068 回覆）
# ＋9,600 值效果生成→配對→12→8 全流程（0xF069 PIPEBENCH，0xF06A 回覆）。
#
# 這一支刻意**不屬於** components/*/test_*：
#   · 不是 NC_ENABLE_TESTS 的 58 條測試指令之一；
#   · 沒有 0xF060–0xF064、network、ncm、量測指令；
#   · 由 CONFIG_NETCORE_EFFECT_BENCH 開關，release 映像也可以有。
# CONFIG 關閉時 nc_effect_bench.c 內容整段 #if 掉，component 只是一個空 object，
# 不會登記指令表／任務。
idf_component_register(
    SRCS "nc_effect_bench.c" "nc_psram_bench.c" "nc_pipe_bench.c"
    REQUIRES nc4 pixel task
    PRIV_REQUIRES freertos esp_timer heap log esp_mm
    WHOLE_ARCHIVE
)
