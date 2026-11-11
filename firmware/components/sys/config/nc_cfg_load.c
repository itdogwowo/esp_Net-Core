// JSON → struct 的解析層。
//
// 這裡是「格式」的唯一入口：欄位的名字、型別、範圍、預設都在這幾個函式裡。
// 呼叫端（main / nc_task）只讀 struct，不再自己 cJSON_GetObjectItem。
//
// 解析規則（固定，不逐個欄位自訂）：
//   缺欄位          → 預設          （defaulted++）
//   型別錯／超範圍   → 預設          （invalid++）
//   不認識的鍵      → 忽略          （unknown++，向前兼容）
//   version 不符    → 整層用預設     （bad_version=1）
//   整份壞 JSON     → 整層用預設     （bad_json=1）
//   檔案不存在      → 整層用預設     （missing=1）
// 以上全部**不會寫回用戶的檔**。
#include "nc_cfg.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"

static const char *TAG = "cfg";

static nc_system_cfg_t s_sys;
static nc_tasks_cfg_t  s_tasks;
static nc_cfg_stat_t   s_sys_stat;
static nc_cfg_stat_t   s_tasks_stat;
static nc_hardware_cfg_t s_hw;
static nc_cfg_stat_t   s_hw_stat;
static bool s_loaded;

// ───────────────────────── 小工具 ─────────────────────────

static const cJSON *item(const cJSON *o, const char *key)
{
    return (o != NULL && cJSON_IsObject(o)) ? cJSON_GetObjectItemCaseSensitive(o, key) : NULL;
}

static int32_t get_int(const cJSON *o, const char *key, int32_t def,
                       int32_t lo, int32_t hi, nc_cfg_stat_t *st)
{
    const cJSON *v = item(o, key);
    if (v == NULL) {
        st->defaulted++;
        return def;
    }
    if (!cJSON_IsNumber(v)) {
        st->invalid++;
        return def;
    }
    const double d = v->valuedouble;
    if (!(d >= (double)lo && d <= (double)hi)) {
        st->invalid++;
        return def;
    }
    return (int32_t)d;
}

// 選填腳位：null／缺欄位都當「未設定」，不計 invalid（hardware.json 用 null 表示沒接）。
static int8_t get_pin_opt(const cJSON *o, const char *key, nc_cfg_stat_t *st)
{
    const cJSON *v = item(o, key);
    if (v == NULL || cJSON_IsNull(v)) {
        st->defaulted++;
        return -1;
    }
    if (!cJSON_IsNumber(v)) {
        st->invalid++;
        return -1;
    }
    const int x = v->valueint;
    if (x < -1 || x > NC_CFG_GPIO_MAX) {
        st->invalid++;
        return -1;
    }
    return (int8_t)x;
}

static bool get_bool(const cJSON *o, const char *key, bool def, nc_cfg_stat_t *st)
{
    const cJSON *v = item(o, key);
    if (v == NULL) {
        st->defaulted++;
        return def;
    }
    if (cJSON_IsBool(v)) {
        return cJSON_IsTrue(v);
    }
    if (cJSON_IsNumber(v) && (v->valueint == 0 || v->valueint == 1)) {
        return v->valueint != 0;
    }
    st->invalid++;
    return def;
}

static void get_str(const cJSON *o, const char *key, char *out, size_t cap,
                    const char *def, nc_cfg_stat_t *st)
{
    const cJSON *v = item(o, key);
    const char *d = (def != NULL) ? def : "";
    if (v == NULL) {
        st->defaulted++;        // 冇寫
        snprintf(out, cap, "%s", d);
        return;
    }
    // 寫了但不是字串 = 型別錯（要計 invalid，唔係 defaulted）——
    // 這兩種在 STATUS 上要分得出來，否則「打錯字」會看起來像「冇填」。
    if (!cJSON_IsString(v) || v->valuestring == NULL) {
        st->invalid++;
        snprintf(out, cap, "%s", d);
        return;
    }
    if (strlen(v->valuestring) >= cap) {
        st->invalid++;          // 放唔落就唔截斷：截斷會變成另一個名
        snprintf(out, cap, "%s", d);
        return;
    }
    snprintf(out, cap, "%s", v->valuestring);
}

static void note_unknown(const cJSON *o, const char *const *known, size_t n,
                         nc_cfg_stat_t *st)
{
    if (!cJSON_IsObject(o)) {
        return;
    }
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, o) {
        if (it->string == NULL) {
            continue;
        }
        bool ok = false;
        for (size_t i = 0; i < n; i++) {
            if (strcmp(it->string, known[i]) == 0) {
                ok = true;
                break;
            }
        }
        if (!ok) {
            st->unknown++;
        }
    }
}

static bool kind_parse(const char *s, nc_link_kind_t *out)
{
    static const char *const names[NC_LINK_KIND_MAX] = {
        [NC_LINK_CDC] = "cdc", [NC_LINK_UART] = "uart", [NC_LINK_TCP] = "tcp",
        [NC_LINK_WS] = "ws",   [NC_LINK_UDP] = "udp",
        [NC_LINK_USBJTAG] = "usb_jtag",
    };
    if (s == NULL || out == NULL) {
        return false;
    }
    for (int i = 0; i < NC_LINK_KIND_MAX; i++) {
        if (names[i] != NULL && strcasecmp(s, names[i]) == 0) {
            *out = (nc_link_kind_t)i;
            return true;
        }
    }
    return false;
}

// 讀一個 rx 數值欄位：只有「config 寫了而且合法」才回 true（給 rx_mask 用）。
static bool rx_take(const cJSON *rx, const char *key, int32_t lo, int32_t hi,
                    uint16_t *dst, nc_cfg_stat_t *st)
{
    const cJSON *v = item(rx, key);
    if (v == NULL) {
        st->defaulted++;
        return false;
    }
    if (!cJSON_IsNumber(v)) {
        st->invalid++;
        return false;
    }
    const double d = v->valuedouble;
    if (!(d >= (double)lo && d <= (double)hi)) {
        st->invalid++;
        return false;
    }
    *dst = (uint16_t)d;
    return true;
}

// ───────────────────────── system ─────────────────────────

static void parse_pools(const cJSON *root, nc_system_cfg_t *out, nc_cfg_stat_t *st)
{
    const cJSON *arr = item(item(root, "nc4"), "pool");
    if (arr == NULL) {
        st->defaulted++;
        return;
    }
    if (!cJSON_IsArray(arr)) {
        st->invalid++;
        return;
    }

    out->n_pools = 0;
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, arr) {
        if (out->n_pools >= NC_CFG_MAX_POOLS) {
            st->invalid++;
            break;
        }
        nc_cfg_pool_t p;
        memset(&p, 0, sizeof(p));

        const cJSON *nm = item(it, "name");
        if (!cJSON_IsString(nm) || nm->valuestring == NULL ||
            nm->valuestring[0] == '\0' || strlen(nm->valuestring) >= NC_CFG_NAME_MAX) {
            st->invalid++;
            continue;
        }
        snprintf(p.name, sizeof(p.name), "%s", nm->valuestring);
        p.size  = (uint16_t)get_int(it, "size", 1024, 64, NC_CFG_REGION_MAX, st);
        p.count = (uint8_t)get_int(it, "count", 4, 1, 32, st);
        p.enable = get_bool(it, "enable", true, st);

        const cJSON *w = item(it, "where");
        if (w == NULL) {
            st->defaulted++;
            p.where = 0;
        } else if (cJSON_IsString(w) && w->valuestring != NULL &&
                   strcmp(w->valuestring, "psram") == 0) {
            p.where = 1;
        } else if (cJSON_IsString(w) && w->valuestring != NULL &&
                   strcmp(w->valuestring, "internal") == 0) {
            p.where = 0;
        } else {
            st->invalid++;
            p.where = 0;
        }

        out->pools[out->n_pools++] = p;
    }
}

static void parse_links(const cJSON *root, nc_system_cfg_t *out, nc_cfg_stat_t *st)
{
    const cJSON *arr = item(item(root, "nc4"), "links");
    if (arr == NULL) {
        st->defaulted++;        // 整段缺失 → 保留預設的兩條（CDC / UART）
        return;
    }
    if (!cJSON_IsArray(arr)) {
        st->invalid++;
        return;
    }

    out->n_links = 0;           // 有寫 links[] 就以它為準（空陣列 = 全部關閉）
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, arr) {
        if (out->n_links >= NC_CFG_MAX_LINKS) {
            st->invalid++;      // 超出上限：多出來的不建立（fail-visible）
            break;
        }
        nc_link_cfg_t l;
        memset(&l, 0, sizeof(l));

        const cJSON *nm = item(it, "name");
        if (!cJSON_IsString(nm) || nm->valuestring == NULL || nm->valuestring[0] == '\0' ||
            strlen(nm->valuestring) >= NC_CFG_NAME_MAX) {
            st->invalid++;      // 冇名字的線沒有意義
            continue;
        }
        snprintf(l.name, sizeof(l.name), "%s", nm->valuestring);

        const cJSON *kd = item(it, "kind");
        if (kd != NULL && cJSON_IsString(kd) && kd->valuestring != NULL) {
            if (!kind_parse(kd->valuestring, &l.kind)) {
                st->invalid++;
                continue;
            }
        } else {
            nc_link_kind_t k;
            if (kind_parse(l.name, &k)) {
                l.kind = k;     // 冇寫 kind → 由名字推（CDC → cdc）
                st->defaulted++;
            } else {
                st->invalid++;
                continue;
            }
        }

        l.enable = get_bool(it, "enable", true, st);
        l.port = (uint16_t)get_int(it, "port", 0, 0, 65535, st);

        nc_cfg_rx_default(l.kind, &l.rx);
        const cJSON *rx = item(item(it, "cfg"), "rx");
        if (rx == NULL) {
            st->defaulted++;    // 只寫 name/kind → rx 用該 kind 的量測預設
        } else if (!cJSON_IsObject(rx)) {
            st->invalid++;
        } else {
            uint16_t v;
            if (rx_take(rx, "region", 64, NC_CFG_REGION_MAX, &v, st)) {
                l.rx.region = v;
                l.rx_mask |= NC_CFG_RX_REGION;
            }
            if (rx_take(rx, "chunk", 16, NC_CFG_REGION_MAX, &v, st)) {
                l.rx.chunk = v;
                l.rx_mask |= NC_CFG_RX_CHUNK;
            }
            if (rx_take(rx, "budget", 1, 64, &v, st)) {
                l.rx.budget = (uint8_t)v;
                l.rx_mask |= NC_CFG_RX_BUDGET;
            }
            if (rx_take(rx, "slots", 1, 16, &v, st)) {
                l.rx.slots = (uint8_t)v;
                l.rx_mask |= NC_CFG_RX_SLOTS;
            }
            if (rx_take(rx, "slot_size", 16, NC_CFG_REGION_MAX, &v, st)) {
                l.rx.slot_size = v;
                l.rx_mask |= NC_CFG_RX_SLOT_SIZE;
            }
            // chunk 大過一格沒有意義：PUSH 一次只搬一格
            if (l.rx.chunk > l.rx.slot_size) {
                l.rx.chunk = l.rx.slot_size;
            }
        }

        out->links[out->n_links++] = l;
    }
}

static void parse_routes(const cJSON *root, nc_system_cfg_t *out, nc_cfg_stat_t *st)
{
    const cJSON *arr = item(item(root, "nc4"), "routes");
    if (arr == NULL) {
        st->defaulted++;
        return;
    }
    if (!cJSON_IsArray(arr)) {
        st->invalid++;
        return;
    }

    out->n_routes = 0;
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, arr) {
        if (out->n_routes >= NC_CFG_MAX_ROUTES) {
            st->invalid++;
            break;
        }
        nc_route_cfg_t r;
        memset(&r, 0, sizeof(r));

        const cJSON *in = item(it, "in");
        if (!cJSON_IsString(in) || in->valuestring == NULL || in->valuestring[0] == '\0' ||
            strlen(in->valuestring) >= NC_CFG_NAME_MAX) {
            st->invalid++;
            continue;
        }
        snprintf(r.in, sizeof(r.in), "%s", in->valuestring);

        const cJSON *outs = item(it, "out");
        if (outs == NULL) {
            st->defaulted++;    // 冇 out = 丟棄（合法）
        } else if (!cJSON_IsArray(outs)) {
            st->invalid++;
        } else {
            const cJSON *o = NULL;
            cJSON_ArrayForEach(o, outs) {
                if (r.n_out >= NC_CFG_MAX_OUT) {
                    st->invalid++;   // 超出上限：多的丟掉（會記錄）
                    break;
                }
                if (!cJSON_IsString(o) || o->valuestring == NULL ||
                    strlen(o->valuestring) >= NC_CFG_NAME_MAX) {
                    st->invalid++;
                    continue;
                }
                snprintf(r.out[r.n_out], sizeof(r.out[0]), "%s", o->valuestring);
                r.n_out++;
            }
        }

        out->routes[out->n_routes++] = r;
    }
}

// ───────────────────────── hardware ─────────────────────────
// 每一段一個函式，apply_hardware 只負責「按段叫」；加一段 = 加一個函式 + 一行。
// 這個形狀是刻意跟 hardware-config.md 的「preset / merge」對齊的：
// 之後每一段都會變成「preset 表 + 逐欄位覆蓋」，而不是在這裡逐個 get_int。

// ETH（RMII）：{ "ETH": {"enable":1,"mdc":31,"mdio":52,"ref_clk":50,
//                        "phy_type":"IP101","phy_addr":1} }
// spi / cs / rst / int 是 SPI 接線的 PHY 用的欄位：認得（不計 unknown）但唔解析 ——
// IDF 6 的 RMII 驅動不需要它們，要用再補型別（現在解析了也只是假裝有作用）。
static void apply_eth(const cJSON *root, nc_hardware_cfg_t *out, nc_cfg_stat_t *st)
{
    nc_eth_cfg_t *e = &out->eth;
    const cJSON *o = item(root, "ETH");
    if (o == NULL) {
        st->defaulted++;        // 缺整段 → present 保持 0（呼叫端用內建腳位）
        return;
    }
    if (!cJSON_IsObject(o)) {
        st->invalid++;          // 型別錯一樣當「冇講」，唔會變成「關閉」
        return;
    }
    e->present = true;          // 有寫就算數 —— 即使 enable=0（明確關閉）
    static const char *const keys[] = {
        "enable", "mdc", "mdio", "ref_clk", "phy_type", "phy_addr",
        "spi", "cs", "rst", "int", "_comment",
    };
    note_unknown(o, keys, sizeof(keys) / sizeof(keys[0]), st);

    e->enable   = get_bool(o, "enable", false, st);
    e->mdc      = (int8_t)get_int(o, "mdc", -1, -1, NC_CFG_GPIO_MAX, st);
    e->mdio     = (int8_t)get_int(o, "mdio", -1, -1, NC_CFG_GPIO_MAX, st);
    e->ref_clk  = (int8_t)get_int(o, "ref_clk", -1, -1, NC_CFG_GPIO_MAX, st);
    e->phy_addr = (uint8_t)get_int(o, "phy_addr", 1, 0, 31, st);
    get_str(o, "phy_type", e->phy_type, sizeof(e->phy_type), "IP101", st);
}

// SDIO：{ "SDIO": { "enable":1, "list":[ {"freq":40000000,"width":4,
//                   "GPIO":{"sck":7,"cmd":6,"data":[15,16,4,5]}} ] } }
static void apply_sdio(const cJSON *root, nc_hardware_cfg_t *out, nc_cfg_stat_t *st)
{
    const cJSON *sdio = item(root, "SDIO");
    if (sdio == NULL) {
        st->defaulted++;
        return;
    }
    if (!cJSON_IsObject(sdio)) {
        st->invalid++;
        return;
    }
    out->n_sdio = 0;
    if (!get_bool(sdio, "enable", false, st)) {
        return;
    }
    const cJSON *list = item(sdio, "list");
    if (!cJSON_IsArray(list)) {
        st->invalid++;
        return;
    }
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, list) {
        if (out->n_sdio >= NC_CFG_MAX_SDIO) {
            st->invalid++;
            break;
        }
        nc_sdio_cfg_t e;
        memset(&e, 0, sizeof(e));
        e.enable  = true;
        e.freq_hz = (uint32_t)get_int(it, "freq", 40000000, 100000, 80000000, st);
        e.width   = (uint8_t)get_int(it, "width", 4, 1, 4, st);
        const cJSON *g = item(it, "GPIO");
        e.clk = (int8_t)get_int(g, "sck", -1, -1, NC_CFG_GPIO_MAX, st);
        e.cmd = (int8_t)get_int(g, "cmd", -1, -1, NC_CFG_GPIO_MAX, st);
        const cJSON *arr = item(g, "data");
        for (int i = 0; i < 4; i++) {
            const cJSON *v = cJSON_IsArray(arr) ? cJSON_GetArrayItem(arr, i) : NULL;
            e.d[i] = (cJSON_IsNumber(v) && v->valueint >= 0 && v->valueint <= NC_CFG_GPIO_MAX)
                         ? (int8_t)v->valueint : -1;
        }
        out->sdio[out->n_sdio++] = e;
    }
}

// PIN：只有 enable 時才建立；GPIO null 跳過。
static void apply_pin(const cJSON *root, nc_hardware_cfg_t *out, nc_cfg_stat_t *st)
{
    const cJSON *pin = item(root, "PIN");
    if (pin == NULL) {
        st->defaulted++;
        return;
    }
    if (!cJSON_IsObject(pin)) {
        st->invalid++;
        return;
    }
    if (!get_bool(pin, "enable", false, st)) {
        out->n_pin = 0;
        return;
    }
    const cJSON *list = item(pin, "list");
    if (!cJSON_IsArray(list)) {
        st->defaulted++;
        return;
    }
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, list) {
        if (out->n_pin >= NC_CFG_MAX_PIN) {
            st->invalid++;
            break;
        }
        const int8_t pinno = get_pin_opt(it, "GPIO", st);
        if (pinno < 0) {
            continue;
        }
        nc_pin_cfg_t e;
        memset(&e, 0, sizeof(e));
        e.pin = pinno;
        char mode[8];
        get_str(it, "mode", mode, sizeof(mode), "OUT", st);
        e.mode = (strcasecmp(mode, "IN") == 0) ? 1u : 0u;
        char pull[8];
        get_str(it, "pull", pull, sizeof(pull), "NONE", st);
        e.pull = (strcasecmp(pull, "UP") == 0) ? 1u :
                 (strcasecmp(pull, "DOWN") == 0) ? 2u : 0u;
        e.initial = (uint8_t)get_int(it, "initial", 0, 0, 1, st);
        get_str(it, "label", e.label, sizeof(e.label), "", st);
        out->pin[out->n_pin++] = e;
    }
}

// PWM：LEDC。enable=0 → 保留 0 條；有 enable 才逐條建立。
static void apply_pwm(const cJSON *root, nc_hardware_cfg_t *out, nc_cfg_stat_t *st)
{
    const cJSON *pwm = item(root, "PWM");
    if (pwm == NULL) {
        st->defaulted++;
        return;
    }
    if (!cJSON_IsObject(pwm)) {
        st->invalid++;
        return;
    }
    if (!get_bool(pwm, "enable", false, st)) {
        out->n_pwm = 0;
        return;
    }
    const cJSON *list = item(pwm, "list");
    if (!cJSON_IsArray(list)) {
        st->defaulted++;
        return;
    }
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, list) {
        if (out->n_pwm >= NC_CFG_MAX_PWM) {
            st->invalid++;
            break;
        }
        const int8_t pin = get_pin_opt(it, "GPIO", st);
        if (pin < 0) {
            continue;
        }
        nc_pwm_cfg_t e;
        memset(&e, 0, sizeof(e));
        e.pin = pin;
        e.freq_hz = (uint32_t)get_int(it, "freq", 1000, 1, 40000000, st);
        e.resolution_bits = (uint8_t)get_int(it, "resolution", 12, 1, 14, st);
        const uint32_t max_duty = (1u << e.resolution_bits) - 1u;
        e.duty = (uint16_t)get_int(it, "duty", 0, 0, (int32_t)max_duty, st);
        e.d_stay = (uint16_t)get_int(it, "dStay", 0, 0, (int32_t)max_duty, st);
        get_str(it, "label", e.label, sizeof(e.label), "", st);
        out->pwm[out->n_pwm++] = e;
    }
}

// UART：照舊專案；en >= 0 = RS485 DE 腳。
static void apply_uart(const cJSON *root, nc_hardware_cfg_t *out, nc_cfg_stat_t *st)
{
    const cJSON *uart = item(root, "UART");
    if (uart == NULL) {
        st->defaulted++;
        return;
    }
    if (!cJSON_IsObject(uart)) {
        st->invalid++;
        return;
    }
    if (!get_bool(uart, "enable", false, st)) {
        out->n_uart = 0;
        return;
    }
    const cJSON *list = item(uart, "list");
    if (!cJSON_IsArray(list)) {
        st->defaulted++;
        return;
    }
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, list) {
        if (out->n_uart >= NC_CFG_MAX_UART) {
            st->invalid++;
            break;
        }
        const cJSON *g = item(it, "GPIO");
        if (!cJSON_IsObject(g)) {
            st->invalid++;
            continue;
        }
        nc_uart_cfg_t e;
        memset(&e, 0, sizeof(e));
        e.enable = true;
        e.id = (uint8_t)get_int(it, "id", 1, 0, 2, st);
        e.baud = (uint32_t)get_int(it, "baudrate", 115200, 300, 5000000, st);
        e.rxbuf = (uint32_t)get_int(it, "rxbuf", 16384, 256, 65536, st);
        e.txbuf = (uint32_t)get_int(it, "txbuf", 16384, 256, 65536, st);
        e.tx = get_pin_opt(g, "tx", st);
        e.rx = get_pin_opt(g, "rx", st);
        e.en = get_pin_opt(g, "en", st);
        e.en_settle_ms = (uint16_t)get_int(it, "en_settle_ms", 1, 0, 1000, st);
        if (e.tx < 0 || e.rx < 0) {
            st->defaulted++;     // 未接線 → 這條不建立
            continue;
        }
        out->uart[out->n_uart++] = e;
    }
}

esp_err_t nc_cfg_apply_hardware(const cJSON *root, nc_hardware_cfg_t *out, nc_cfg_stat_t *st)
{
    if (out == NULL || st == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = *nc_cfg_defaults_hardware();
    // system / tasks 兩個 apply_* 都有這一行，hardware 以前漏了：boot 路徑的
    // s_hw_stat 是 static 所以看不出來，任何非 static 的呼叫端（host test）拿到的是
    // 未初始化的計數 —— 看起來像「解析出錯」，其實是沒人清過。
    memset(st, 0, sizeof(*st));
    if (root == NULL) {
        st->missing = 1;
        return ESP_OK;
    }
    if (!cJSON_IsObject(root)) {
        st->bad_json = 1;
        return ESP_OK;
    }
    const cJSON *ver = item(root, "version");
    if (ver != NULL && cJSON_IsNumber(ver) && ver->valueint != NC_CFG_SCHEMA_VERSION) {
        st->bad_version = 1;
        return ESP_OK;
    }
    static const char *const root_keys[] = {
        "version", "_comment", "_gpio_convention",
        "ETH", "PIN", "SPI", "I2C", "I80", "UART", "I2S", "PWM", "SDIO", "ENC", "XL9555",
    };
    note_unknown(root, root_keys, sizeof(root_keys) / sizeof(root_keys[0]), st);

    apply_eth(root, out, st);    // 已型別化的段：改它 = 改行為
    apply_sdio(root, out, st);
    apply_pin(root, out, st);    // GPIO owner 開機套用
    apply_pwm(root, out, st);    // LEDC
    apply_uart(root, out, st);   // UART / RS485
    return ESP_OK;
}

// SD 卡**不在這一層**：它是一條匯流排上的裝置，enable／腳位／頻率／寬度全部由
// hardware.json 的 SDIO 決定（跟 ETH 一樣只出現一次）。掛載點固定 /sd。
// 以前 system.json 有一份 { enable, bus:{type,index} } —— 那是「SPI 與 SDIO 二選一」
// 年代的東西，兩者都係 SD 之後就變成純粹的重複，已刪。

esp_err_t nc_cfg_apply_system(const cJSON *root, nc_system_cfg_t *out, nc_cfg_stat_t *st)
{
    if (out == NULL || st == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = *nc_cfg_defaults_system();
    memset(st, 0, sizeof(*st));

    if (root == NULL) {
        st->missing = 1;
        return ESP_OK;
    }
    if (!cJSON_IsObject(root)) {
        st->bad_json = 1;
        *out = *nc_cfg_defaults_system();
        return ESP_OK;
    }

    const cJSON *ver = item(root, "version");
    if (ver == NULL) {
        st->defaulted++;            // 冇 version → 當 v1（寬容）
    } else if (!cJSON_IsNumber(ver) || ver->valueint != NC_CFG_SCHEMA_VERSION) {
        st->bad_version = 1;        // 唔識的版本 → 整層用預設
        return ESP_OK;
    }

    static const char *const root_keys[] = {
        "version", "_comment", "cID", "frame_interval_ms", "num_pixels", "nc4",
        "log_debug_mode", "usb",
    };
    note_unknown(root, root_keys, sizeof(root_keys) / sizeof(root_keys[0]), st);

    get_str(root, "cID", out->cid, sizeof(out->cid), "", st);
    out->frame_interval_ms = (uint16_t)get_int(root, "frame_interval_ms", 20, 1, 1000, st);
    out->log_debug_mode = (uint8_t)get_int(root, "log_debug_mode", 0, 0, 1, st);
    out->num_pixels = (uint16_t)get_int(root, "num_pixels", 0, 0, 65535, st);

    // ── usb：執行期開關（編譯期有沒有 USB 由 Kconfig 決定，這裡只決定要不要用）──
    // 缺席 = 兩個都 1（跟 nc_cfg_defaults_system 一致）。
    const cJSON *usb = item(root, "usb");
    if (usb != NULL) {
        if (!cJSON_IsObject(usb)) {
            st->invalid++;
        } else {
            static const char *const usb_keys[] = { "msc", "cdc_log" };
            note_unknown(usb, usb_keys, sizeof(usb_keys) / sizeof(usb_keys[0]), st);
            out->usb_msc = (uint8_t)get_int(usb, "msc", out->usb_msc, 0, 1, st);
            out->usb_cdc_log = (uint8_t)get_int(usb, "cdc_log", out->usb_cdc_log, 0, 1, st);
        }
    }

    const cJSON *nc4 = item(root, "nc4");
    if (nc4 == NULL) {
        st->defaulted++;            // 冇 nc4 → 保留預設的通道與路由
        return ESP_OK;
    }
    if (!cJSON_IsObject(nc4)) {
        st->invalid++;
        return ESP_OK;
    }
    static const char *const nc4_keys[] = { "links", "routes", "pool" };
    note_unknown(nc4, nc4_keys, sizeof(nc4_keys) / sizeof(nc4_keys[0]), st);

    parse_links(root, out, st);
    parse_routes(root, out, st);
    parse_pools(root, out, st);
    return ESP_OK;
}

// ───────────────────────── tasks ─────────────────────────

esp_err_t nc_cfg_apply_tasks(const cJSON *root, nc_tasks_cfg_t *out, nc_cfg_stat_t *st)
{
    if (out == NULL || st == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = *nc_cfg_defaults_tasks();
    memset(st, 0, sizeof(*st));

    if (root == NULL) {
        st->missing = 1;
        return ESP_OK;
    }
    if (!cJSON_IsObject(root)) {
        st->bad_json = 1;
        *out = *nc_cfg_defaults_tasks();
        return ESP_OK;
    }

    const cJSON *ver = item(root, "version");
    if (ver == NULL) {
        st->defaulted++;
    } else if (!cJSON_IsNumber(ver) || ver->valueint != NC_CFG_SCHEMA_VERSION) {
        st->bad_version = 1;
        return ESP_OK;
    }

    static const char *const root_keys[] = { "version", "_comment", "tasks" };
    note_unknown(root, root_keys, sizeof(root_keys) / sizeof(root_keys[0]), st);

    const cJSON *arr = item(root, "tasks");
    if (arr == NULL) {
        st->defaulted++;            // 空 = 全部用固件宣告
        return ESP_OK;
    }
    if (!cJSON_IsArray(arr)) {
        st->invalid++;
        return ESP_OK;
    }

    out->n_tasks = 0;
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, arr) {
        if (out->n_tasks >= NC_CFG_MAX_TASKS) {
            st->invalid++;
            break;
        }
        nc_task_cfg_t t;
        memset(&t, 0, sizeof(t));

        const cJSON *nm = item(it, "name");
        if (!cJSON_IsString(nm) || nm->valuestring == NULL || nm->valuestring[0] == '\0' ||
            strlen(nm->valuestring) >= NC_CFG_TASK_NAME_MAX) {
            st->invalid++;
            continue;
        }
        snprintf(t.name, sizeof(t.name), "%s", nm->valuestring);

        // level 要分得出「冇寫」同「寫 0」：冇寫 → 用固件宣告的 level。
        const cJSON *lv = item(it, "level");
        if (lv == NULL) {
            st->defaulted++;
        } else if (cJSON_IsNumber(lv) && lv->valueint >= -1 && lv->valueint <= 7) {
            t.level = (int8_t)lv->valueint;
            t.has_level = true;
        } else {
            st->invalid++;      // 型別錯／超範圍 → 當冇寫
        }

        const cJSON *af = item(it, "affinity");
        if (af == NULL) {
            st->defaulted++;        // 未設定 = (0,0) = 唔跑
        } else if (!cJSON_IsArray(af) || cJSON_GetArraySize(af) != 2) {
            st->invalid++;
        } else {
            for (int k = 0; k < 2; k++) {
                const cJSON *a = cJSON_GetArrayItem(af, k);
                if (cJSON_IsNumber(a) && (a->valueint == 0 || a->valueint == 1)) {
                    t.aff[k] = a->valueint != 0;
                } else {
                    st->invalid++;  // 非 0/1 一律當 0（唔准跑嗰核）
                }
            }
        }

        out->tasks[out->n_tasks++] = t;
    }
    return ESP_OK;
}

// ───────────────────────── 存取 ─────────────────────────

esp_err_t nc_cfg_boot(void)
{
    return nc_cfg_boot_ex(NULL, 0);
}

const nc_system_cfg_t *nc_cfg_system(void)
{
    return &s_sys;
}

const nc_tasks_cfg_t *nc_cfg_tasks(void)
{
    return &s_tasks;
}

const nc_hardware_cfg_t *nc_cfg_hardware(void)
{
    return &s_hw;
}

const nc_cfg_stat_t *nc_cfg_stat_of(const char *layer)
{
    if (layer == NULL) {
        return NULL;
    }
    if (strcmp(layer, "system") == 0) {
        return &s_sys_stat;
    }
    if (strcmp(layer, "tasks") == 0) {
        return &s_tasks_stat;
    }
    if (strcmp(layer, "hardware") == 0) {
        return &s_hw_stat;
    }
    return NULL;
}

const nc_link_cfg_t *nc_cfg_link(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (uint8_t i = 0; i < s_sys.n_links; i++) {
        if (strcmp(s_sys.links[i].name, name) == 0) {
            return &s_sys.links[i];
        }
    }
    return NULL;
}

const nc_route_cfg_t *nc_cfg_route(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (uint8_t i = 0; i < s_sys.n_routes; i++) {
        if (strcmp(s_sys.routes[i].in, name) == 0) {
            return &s_sys.routes[i];
        }
    }
    return NULL;
}

bool nc_cfg_layer_bad(const char *layer)
{
    const nc_cfg_stat_t *st = nc_cfg_stat_of(layer);
    if (st == NULL) {
        return true;
    }
    return st->missing != 0 || st->bad_json != 0 || st->bad_version != 0;
}

// ───────────────────────── 開機載入 ─────────────────────────

// 板級 wiring（build-time）當 base，用戶 hardware.json 逐欄位覆蓋。
// 物件遞迴合併；陣列／純量由用戶整個取代（wiring 的 list 不會被半套覆蓋）。
static void merge_obj(cJSON *dst, const cJSON *src)
{
    if (!cJSON_IsObject(dst) || !cJSON_IsObject(src)) {
        return;
    }
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, src) {
        cJSON *old = cJSON_GetObjectItemCaseSensitive(dst, it->string);
        if (old != NULL && cJSON_IsObject(old) && cJSON_IsObject(it)) {
            merge_obj(old, it);
        } else {
            cJSON *dup = cJSON_Duplicate(it, 1);
            if (dup == NULL) {
                continue;
            }
            if (old != NULL) {
                cJSON_ReplaceItemInObjectCaseSensitive(dst, it->string, dup);
            } else {
                cJSON_AddItemToObject(dst, it->string, dup);
            }
        }
    }
}

esp_err_t nc_cfg_boot_ex(const char *hw_base_json, size_t hw_base_len)
{
    bool exists = false;

    cfg_file_t *f = nc_cfg_open_ex("system", &exists);
    if (f != NULL) {
        (void)nc_cfg_apply_system(nc_cfg_root(f), &s_sys, &s_sys_stat);
        nc_cfg_close(f);
    } else {
        (void)nc_cfg_apply_system(NULL, &s_sys, &s_sys_stat);
        if (exists) {
            s_sys_stat.missing = 0;
            s_sys_stat.bad_json = 1;
        }
    }

    exists = false;
    cJSON *base = NULL;
    if (hw_base_json != NULL && hw_base_len > 0) {
        base = cJSON_ParseWithLength(hw_base_json, hw_base_len);
        if (base != NULL && !cJSON_IsObject(base)) {
            cJSON_Delete(base);
            base = NULL;
        }
        if (base == NULL) {
            ESP_LOGW(TAG, "wiring base JSON 解析失敗；只用用戶 hardware.json");
        }
    }
    f = nc_cfg_open_ex("hardware", &exists);
    cJSON *user = NULL;
    if (f != NULL) {
        user = cJSON_Duplicate(nc_cfg_root(f), 1);
        nc_cfg_close(f);
    }
    if (base != NULL) {
        if (user != NULL) {
            merge_obj(base, user);       // 用戶覆蓋板級 wiring
        }
        (void)nc_cfg_apply_hardware(base, &s_hw, &s_hw_stat);
    } else {
        (void)nc_cfg_apply_hardware(user, &s_hw, &s_hw_stat);
    }
    cJSON_Delete(user);
    cJSON_Delete(base);
    if (exists && user == NULL && f == NULL) {
        s_hw_stat.missing = 0;
        s_hw_stat.bad_json = 1;
    } else if (base != NULL) {
        s_hw_stat.missing = 0;
    }

    exists = false;
    f = nc_cfg_open_ex("tasks", &exists);
    if (f != NULL) {
        (void)nc_cfg_apply_tasks(nc_cfg_root(f), &s_tasks, &s_tasks_stat);
        nc_cfg_close(f);
    } else {
        (void)nc_cfg_apply_tasks(NULL, &s_tasks, &s_tasks_stat);
        if (exists) {
            s_tasks_stat.missing = 0;
            s_tasks_stat.bad_json = 1;
        }
    }

    s_loaded = true;

    ESP_LOGI(TAG, "system: links=%u routes=%u%s%s%s（def=%u inv=%u unk=%u）",
             (unsigned)s_sys.n_links, (unsigned)s_sys.n_routes,
             s_sys_stat.missing ? " 缺檔" : "",
             s_sys_stat.bad_json ? " 壞檔" : "",
             s_sys_stat.bad_version ? " 版本不符" : "",
             (unsigned)s_sys_stat.defaulted, (unsigned)s_sys_stat.invalid,
             (unsigned)s_sys_stat.unknown);
    // ETH 的「未寫」與「關閉」要分得出來：前者用內建腳位，後者真的不啟動。
    const char *eth_state = !s_hw.eth.present ? "未寫(內建)" : (s_hw.eth.enable ? "開" : "關");
    ESP_LOGI(TAG, "hardware: eth=%s mdc=%d mdio=%d refclk=%d %s@%u；sdio=%u%s%s（def=%u inv=%u unk=%u）",
             eth_state, s_hw.eth.mdc, s_hw.eth.mdio, s_hw.eth.ref_clk,
             s_hw.eth.phy_type, (unsigned)s_hw.eth.phy_addr,
             (unsigned)s_hw.n_sdio,
             s_hw_stat.missing ? " 缺檔" : "",
             s_hw_stat.bad_json ? " 壞檔" : "",
             (unsigned)s_hw_stat.defaulted, (unsigned)s_hw_stat.invalid,
             (unsigned)s_hw_stat.unknown);
    ESP_LOGI(TAG, "tasks: %u 條%s%s（def=%u inv=%u unk=%u）",
             (unsigned)s_tasks.n_tasks,
             s_tasks_stat.missing ? " 缺檔" : "",
             s_tasks_stat.bad_json ? " 壞檔" : "",
             (unsigned)s_tasks_stat.defaulted, (unsigned)s_tasks_stat.invalid,
             (unsigned)s_tasks_stat.unknown);
    return ESP_OK;
}

void nc_cfg_json(char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) {
        return;
    }
    snprintf(buf, cap,
             "{\"sys\":{\"miss\":%u,\"bad\":%u,\"ver\":%u,\"def\":%u,\"inv\":%u,\"unk\":%u},"
             "\"tasks\":{\"miss\":%u,\"bad\":%u,\"ver\":%u,\"def\":%u,\"inv\":%u,\"unk\":%u},"
             "\"hw\":{\"miss\":%u,\"bad\":%u,\"ver\":%u,\"def\":%u,\"inv\":%u,\"unk\":%u}}",
             (unsigned)s_sys_stat.missing, (unsigned)s_sys_stat.bad_json,
             (unsigned)s_sys_stat.bad_version, (unsigned)s_sys_stat.defaulted,
             (unsigned)s_sys_stat.invalid, (unsigned)s_sys_stat.unknown,
             (unsigned)s_tasks_stat.missing, (unsigned)s_tasks_stat.bad_json,
             (unsigned)s_tasks_stat.bad_version, (unsigned)s_tasks_stat.defaulted,
             (unsigned)s_tasks_stat.invalid, (unsigned)s_tasks_stat.unknown,
             (unsigned)s_hw_stat.missing, (unsigned)s_hw_stat.bad_json,
             (unsigned)s_hw_stat.bad_version, (unsigned)s_hw_stat.defaulted,
             (unsigned)s_hw_stat.invalid, (unsigned)s_hw_stat.unknown);
}
