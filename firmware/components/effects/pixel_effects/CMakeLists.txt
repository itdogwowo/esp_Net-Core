# pixel_effects（effects 層）：CORE 內建效果的**實作**。
#
# 引擎（契約、registry、interpreter、抖動）在 components/sw/pixel；這裡只放具體效果。
# 規則：docs/03_notes/behavior-taxonomy.md §2.0（三個名）／§3（引擎 vs 實作）。
# 加一個效果 = 加一個 nc_effect_<name>.c ＋ nc_effect_table.c 加一行。
idf_component_register(
    SRCS "nc_effect_zone1.c" "nc_effect_breath.c" "nc_effect_wave.c"
         "nc_effect_solid.c" "nc_effect_ramp.c" "nc_effect_table.c"
    INCLUDE_DIRS "include"
    REQUIRES pixel
)
