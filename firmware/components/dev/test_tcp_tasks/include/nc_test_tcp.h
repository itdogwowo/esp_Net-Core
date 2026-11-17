// TCP 吞吐量測端點（5000–5003）。測試用，受 NC_ENABLE_TESTS gate。
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
void nc_test_tcp_start(void);
#ifdef __cplusplus
}
#endif
