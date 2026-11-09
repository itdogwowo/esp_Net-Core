// NC4 出口登記表（純邏輯、零 IDF 依賴，所以 host 都跑得）。
//
// 呢個檔案只做三件事：驗名、記名、交幀。唔建立 bus、唔配記憶體、唔開任務。
// 佢唔知道咩叫 CDC／UART／TCP —— 嗰啲名字同函式由 boot 時的 transport 自己送上門。

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "nc4_link.h"

// 計數會被兩個核同時改（decode0 / decode1 都可以轉寄）；但係邊際用原子操作。
// MSVC 沒有 __atomic_*，用 Interlocked；其他（GCC/Clang）用 builtin。
#if defined(_MSC_VER)
#include <intrin.h>
#define LINK_ADD(p, v)   ((void)_InterlockedExchangeAdd((volatile long *)(p), (long)(v)))
#define LINK_LOAD(p)     (*(const volatile uint32_t *)(p))
#define LINK_STORE(p, v) ((void)_InterlockedExchange((volatile long *)(p), (long)(v)))
#else
#define LINK_ADD(p, v)   ((void)__atomic_add_fetch((p), (v), __ATOMIC_RELAXED))
#define LINK_LOAD(p)     __atomic_load_n((p), __ATOMIC_RELAXED)
#define LINK_STORE(p, v) ((void)__atomic_store_n((p), (v), __ATOMIC_RELAXED))
#endif

typedef struct {
    char name[NC4_LINK_NAME_MAX];
    nc4_link_write_fn write;
    void *arg;
    uint32_t frames;       // 成功交出的幀數
    uint32_t bytes;        // 成功交出的 bytes
    uint32_t drops;        // 短送／錯誤（整幀沒有交給對方）
    int32_t  last_err;     // 最後一次失敗的 esp_err_t（0 = 未失敗過）
} nc4_link_t;

static nc4_link_t s_links[NC4_LINK_MAX];
static int s_count;

static bool name_ok(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return false;
    }
    // 太長就拒絕，唔截斷：截斷會令 "UART1" 同 "UART2" 靜靜變成同一條。
    return strlen(name) < NC4_LINK_NAME_MAX;
}

static nc4_link_t *find(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_links[i].name, name) == 0) {
            return &s_links[i];
        }
    }
    return NULL;
}

esp_err_t nc4_link_register(const char *name, nc4_link_write_fn write, void *arg)
{
    if (!name_ok(name) || write == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (find(name) != NULL) {
        return ESP_ERR_INVALID_STATE;   // 一條線只有一個出口；重名唔可以靜靜蓋掉
    }
    if (s_count >= NC4_LINK_MAX) {
        return ESP_ERR_NO_MEM;
    }
    nc4_link_t *l = &s_links[s_count];
    memset(l, 0, sizeof(*l));
    memcpy(l->name, name, strlen(name) + 1);
    l->write = write;
    l->arg = arg;
    s_count++;
    return ESP_OK;
}

void nc4_link_reset(void)
{
    memset(s_links, 0, sizeof(s_links));
    s_count = 0;
}

bool nc4_link_has(const char *name)
{
    return find(name) != NULL;
}

size_t nc4_link_count(void)
{
    return (size_t)s_count;
}

esp_err_t nc4_link_send(const char *name, const uint8_t *frame, size_t len)
{
    if (frame == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    nc4_link_t *l = find(name);
    if (l == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    const int w = l->write(l->arg, frame, len);
    if (w < 0) {
        LINK_ADD(&l->drops, 1u);
        LINK_STORE(&l->last_err, (int32_t)ESP_FAIL);
        return ESP_FAIL;
    }
    if ((size_t)w != len) {
        // 短送：對方只收了一部分 → 那半幀已經在線路上，唔可以再補（會變兩幀）。
        // 只能計 drop，讓 STATUS 看得到（轉寄的來源緩衝 handler 返回後就失效）。
        LINK_ADD(&l->drops, 1u);
        LINK_STORE(&l->last_err, (int32_t)ESP_ERR_TIMEOUT);
        return ESP_ERR_TIMEOUT;
    }

    LINK_ADD(&l->frames, 1u);
    LINK_ADD(&l->bytes, (uint32_t)len);
    return ESP_OK;
}

void nc4_link_stats(const char *name, uint32_t *frames, uint32_t *bytes, uint32_t *drops)
{
    const nc4_link_t *l = find(name);
    if (frames != NULL) {
        *frames = (l != NULL) ? LINK_LOAD(&l->frames) : 0;
    }
    if (bytes != NULL) {
        *bytes = (l != NULL) ? LINK_LOAD(&l->bytes) : 0;
    }
    if (drops != NULL) {
        *drops = (l != NULL) ? LINK_LOAD(&l->drops) : 0;
    }
}

void nc4_link_stats_reset(void)
{
    for (int i = 0; i < s_count; i++) {
        LINK_STORE(&s_links[i].frames, 0u);
        LINK_STORE(&s_links[i].bytes, 0u);
        LINK_STORE(&s_links[i].drops, 0u);
        LINK_STORE(&s_links[i].last_err, 0);
    }
}

// 追加式格式化：永遠留一個 NUL；滿咗就唔再寫（唔會越界），回傳實際長度。
static size_t jappend(char *buf, size_t cap, size_t len, const char *fmt, ...)
{
    if (len + 1 >= cap) {
        return len;
    }
    va_list ap;
    va_start(ap, fmt);
    const int w = vsnprintf(buf + len, cap - len, fmt, ap);
    va_end(ap);
    if (w < 0) {
        return len;
    }
    const size_t n = (size_t)w;
    if (n >= cap - len) {
        return cap - 1;
    }
    return len + n;
}

size_t nc4_link_json(char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) {
        return 0;
    }
    size_t len = 0;
    len = jappend(buf, cap, len, "{\"max\":%d,\"links\":[", NC4_LINK_MAX);
    for (int i = 0; i < s_count; i++) {
        const nc4_link_t *l = &s_links[i];
        len = jappend(buf, cap, len,
                      "%s{\"name\":\"%s\",\"frames\":%u,\"bytes\":%u,\"drops\":%u}",
                      (i == 0) ? "" : ",", l->name,
                      (unsigned)LINK_LOAD(&l->frames),
                      (unsigned)LINK_LOAD(&l->bytes),
                      (unsigned)LINK_LOAD(&l->drops));
    }
    len = jappend(buf, cap, len, "]}");
    return len;
}
