// 把 ESP_LOGx 導到 stdout；用函式而不是可變參數巨集，避免 -Werror 的 GNU 擴充警告
#pragma once
void mod_host_log_e(const char *tag, const char *fmt, ...);
void mod_host_log_w(const char *tag, const char *fmt, ...);
void mod_host_log_i(const char *tag, const char *fmt, ...);
#define ESP_LOGE mod_host_log_e
#define ESP_LOGW mod_host_log_w
#define ESP_LOGI mod_host_log_i
#define ESP_LOGD(...) do {} while (0)
