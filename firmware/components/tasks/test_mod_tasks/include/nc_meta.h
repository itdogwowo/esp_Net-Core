// .nct / .nce 的 meta 區塊（模組自我描述）—— 純 C、無依賴，所以可以 host 測試。
//
// 位置：manifest 的 reserved（off 34 = meta_size u16、off 36 = meta_off u32）。
// 佈局（540 B，全部 little-endian）：
//   +0    name[24]
//   +24   desc[64]
//   +88   usage[64]
//   +152  n_requires u8
//   +153  n_provides u8
//   +154  reserved[2]
//   +156  requires[8] × 24 B
//   +348  provides[8] × 24 B
//
// 每個 entry（24 B）：
//   +0  type u8（0=buf 1=cmd 2=hw 3=provide 4=module）
//   +1  flags u8（bit0 = exclusive）
//   +2  bytes u16
//   +4  name[20]
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NC_META_NAME_MAX   24
#define NC_META_DESC_MAX   64
#define NC_META_USAGE_MAX  64
#define NC_META_MAX_REQ    8
#define NC_META_MAX_PROV   8
#define NC_META_ENTRY_SIZE 24
#define NC_META_SIZE       (24 + 64 + 64 + 4 + \
                            NC_META_MAX_REQ * NC_META_ENTRY_SIZE + \
                            NC_META_MAX_PROV * NC_META_ENTRY_SIZE)

typedef struct {
    uint8_t  type;      // 0=buf 1=cmd 2=hw 3=provide 4=module
    uint8_t  flags;     // bit0 = exclusive
    uint16_t bytes;
    char     name[20];
} nc_meta_entry_t;

typedef struct {
    char name[NC_META_NAME_MAX];
    char desc[NC_META_DESC_MAX];
    char usage[NC_META_USAGE_MAX];
    int  n_requires;
    nc_meta_entry_t requires[NC_META_MAX_REQ];
    int  n_provides;
    nc_meta_entry_t provides[NC_META_MAX_PROV];
} nc_meta_t;

bool nc_meta_parse(const void *buf, size_t len, nc_meta_t *out);

#ifdef __cplusplus
}
#endif
