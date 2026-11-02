// 把 ESP_LOGx 導到 stdout/stderr；用函式而不是可變參數巨集，避免 -Werror 的 GNU 擴充警告
#pragma once
void fx_host_log_e(const char *tag, const char *fmt, ...);
void fx_host_log_w(const char *tag, const char *fmt, ...);
void fx_host_log_i(const char *tag, const char *fmt, ...);
#define ESP_LOGE fx_host_log_e
#define ESP_LOGW fx_host_log_w
#define ESP_LOGI fx_host_log_i
#define ESP_LOGD(...) do {} while (0)
