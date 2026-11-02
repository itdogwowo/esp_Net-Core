// .fxe manifest 解碼的 host 測試（不需要板子、不需要 IDF）
//
// 驗的是「模組自我宣告」那張表上的每一條：合法宣告 / 壞 magic / 版本 /
// kind / core / layer / prio / stack / fps / px / mem / budget / 指令表。
// 純 C、零依賴，所以 Mac 或 Windows 都能跑。
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nc_fx_manifest.h"

static int s_fail;
static int s_case;

static void check_impl(int cond, int line, const char *what)
{
    printf("  %s  %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) {
        printf("        (%s:%d)\n", __FILE__, line);
        s_fail++;
    }
}

#define CHECK(cond, what) check_impl((cond), __LINE__, (what))

static void case_begin(const char *name)
{
    s_case++;
    printf("[%d] %s\n", s_case, name);
}

// ── 造 manifest ──
static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint8_t  kind;
    int8_t   core;
    uint8_t  layer;
    uint8_t  prio;
    uint16_t fps;
    uint16_t px;
    uint32_t stack;
    uint32_t budget_us;
    uint32_t mem_bytes;
    uint32_t cmds_off;
    uint16_t ncmds;
} spec_t;

static spec_t good_spec(void)
{
    return (spec_t){
        .magic = NC_FXM_MAGIC, .version = NC_FXM_VERSION, .kind = NC_FXM_KIND_EFFECT,
        .core = 1, .layer = 3, .prio = 3, .fps = 30, .px = 676,
        .stack = 4096, .budget_us = 8000, .mem_bytes = 65536,
        .cmds_off = 0, .ncmds = 0,
    };
}

static size_t build(uint8_t *buf, spec_t s)
{
    memset(buf, 0, NC_FXM_HDR_SIZE);
    put_u32(buf + 0, s.magic);
    put_u16(buf + 4, s.version);
    buf[6] = s.kind;
    buf[7] = (uint8_t)s.core;
    buf[8] = s.layer;
    buf[9] = s.prio;
    put_u16(buf + 10, s.fps);
    put_u16(buf + 12, s.px);
    put_u16(buf + 14, 0);             // flags
    put_u32(buf + 16, s.stack);
    put_u32(buf + 20, s.budget_us);
    put_u32(buf + 24, s.mem_bytes);
    put_u32(buf + 28, s.cmds_off);
    put_u16(buf + 32, s.ncmds);
    return NC_FXM_HDR_SIZE;
}

int main(void)
{
    uint8_t buf[256];
    nc_fxm_t m;

    case_begin("合法宣告：effect / core 1 / layer 3 / 30 fps / 676 px");
    build(buf, good_spec());
    CHECK(nc_fxm_parse(buf, NC_FXM_HDR_SIZE, NC_FXM_HDR_SIZE, &m) == NC_FXM_OK, "解析成功");
    CHECK(m.kind == NC_FXM_KIND_EFFECT, "kind=effect");
    CHECK(m.core == 1, "core=1");
    CHECK(m.layer == 3, "layer=3");
    CHECK(m.prio == 3, "prio=3");
    CHECK(m.fps == 30, "fps=30");
    CHECK(m.px == 676, "px=676");
    CHECK(m.stack == 4096, "stack=4096");
    CHECK(m.budget_us == 8000, "budget_us=8000");
    CHECK(m.mem_bytes == 65536, "mem_bytes=65536");
    CHECK(m.ncmds == 0, "沒有指令");

    case_begin("core = -1（不限）與 core = 0 都合法");
    spec_t s = good_spec();
    s.core = -1;
    build(buf, s);
    CHECK(nc_fxm_parse(buf, NC_FXM_HDR_SIZE, NC_FXM_HDR_SIZE, &m) == NC_FXM_OK, "core=-1 收");
    s.core = 0;
    build(buf, s);
    CHECK(nc_fxm_parse(buf, NC_FXM_HDR_SIZE, NC_FXM_HDR_SIZE, &m) == NC_FXM_OK, "core=0 收");

    case_begin("kind 只能是 normal / effect（service 與 task 是同一件事）");
    s = good_spec();
    s.kind = NC_FXM_KIND_SYSTEM;
    build(buf, s);
    CHECK(nc_fxm_parse(buf, NC_FXM_HDR_SIZE, NC_FXM_HDR_SIZE, &m) == NC_FXM_ERR_KIND,
          "system 被拒（載入的檔案不能自稱系統層）");
    s.kind = NC_FXM_KIND_NORMAL;
    build(buf, s);
    CHECK(nc_fxm_parse(buf, NC_FXM_HDR_SIZE, NC_FXM_HDR_SIZE, &m) == NC_FXM_OK, "normal 收");
    s.kind = NC_FXM_KIND_EFFECT;
    build(buf, s);
    CHECK(nc_fxm_parse(buf, NC_FXM_HDR_SIZE, NC_FXM_HDR_SIZE, &m) == NC_FXM_OK, "effect 收");
    s.kind = 3;
    build(buf, s);
    CHECK(nc_fxm_parse(buf, NC_FXM_HDR_SIZE, NC_FXM_HDR_SIZE, &m) == NC_FXM_ERR_KIND,
          "kind=3 被拒（未定義）");

    case_begin("壞 magic / 壞版本 / 太短");
    s = good_spec();
    s.magic = 0xDEADBEEF;
    build(buf, s);
    CHECK(nc_fxm_parse(buf, NC_FXM_HDR_SIZE, NC_FXM_HDR_SIZE, &m) == NC_FXM_ERR_MAGIC,
          "magic 不對");
    s = good_spec();
    s.version = 2;
    build(buf, s);
    CHECK(nc_fxm_parse(buf, NC_FXM_HDR_SIZE, NC_FXM_HDR_SIZE, &m) == NC_FXM_ERR_VERSION,
          "version=2 被拒（不猜）");
    build(buf, good_spec());
    CHECK(nc_fxm_parse(buf, NC_FXM_HDR_SIZE - 1, NC_FXM_HDR_SIZE, &m) == NC_FXM_ERR_MAGIC,
          "長度不足 40 B 被拒");

    case_begin("範圍檢查：core / layer / prio / stack");
    s = good_spec();
    s.core = 2;
    build(buf, s);
    CHECK(nc_fxm_parse(buf, NC_FXM_HDR_SIZE, NC_FXM_HDR_SIZE, &m) == NC_FXM_ERR_CORE,
          "core=2 被拒");
    s = good_spec();
    s.layer = 8;
    build(buf, s);
    CHECK(nc_fxm_parse(buf, NC_FXM_HDR_SIZE, NC_FXM_HDR_SIZE, &m) == NC_FXM_ERR_LAYER,
          "layer=8 被拒（只有 0–7）");
    s = good_spec();
    s.prio = 0;
    build(buf, s);
    CHECK(nc_fxm_parse(buf, NC_FXM_HDR_SIZE, NC_FXM_HDR_SIZE, &m) == NC_FXM_ERR_PRIO,
          "prio=0 被拒（那是 idle）");
    s = good_spec();
    s.prio = 25;
    build(buf, s);
    CHECK(nc_fxm_parse(buf, NC_FXM_HDR_SIZE, NC_FXM_HDR_SIZE, &m) == NC_FXM_ERR_PRIO,
          "prio=25 被拒");
    s = good_spec();
    s.stack = 512;
    build(buf, s);
    CHECK(nc_fxm_parse(buf, NC_FXM_HDR_SIZE, NC_FXM_HDR_SIZE, &m) == NC_FXM_ERR_STACK,
          "stack=512 被拒（太小）");
    s = good_spec();
    s.stack = 0;
    build(buf, s);
    CHECK(nc_fxm_parse(buf, NC_FXM_HDR_SIZE, NC_FXM_HDR_SIZE, &m) == NC_FXM_OK,
          "stack=0 收（用預設）");

    case_begin("範圍檢查：fps / px / mem / budget");
    s = good_spec();
    s.fps = 300;
    build(buf, s);
    CHECK(nc_fxm_parse(buf, NC_FXM_HDR_SIZE, NC_FXM_HDR_SIZE, &m) == NC_FXM_ERR_FPS,
          "fps=300 被拒");
    s = good_spec();
    s.px = 5000;
    build(buf, s);
    CHECK(nc_fxm_parse(buf, NC_FXM_HDR_SIZE, NC_FXM_HDR_SIZE, &m) == NC_FXM_ERR_PX,
          "px=5000 被拒（上限 4096）");
    s = good_spec();
    s.mem_bytes = 200000;
    build(buf, s);
    CHECK(nc_fxm_parse(buf, NC_FXM_HDR_SIZE, NC_FXM_HDR_SIZE, &m) == NC_FXM_ERR_MEM,
          "mem=200 KB 被拒（上限 128 KB）");
    s = good_spec();
    s.budget_us = 200000;
    build(buf, s);
    CHECK(nc_fxm_parse(buf, NC_FXM_HDR_SIZE, NC_FXM_HDR_SIZE, &m) == NC_FXM_ERR_BUDGET,
          "budget=200 ms 被拒");

    case_begin("指令表：位移與長度");
    s = good_spec();
    s.ncmds = 2;
    s.cmds_off = 0;
    build(buf, s);
    CHECK(nc_fxm_parse(buf, NC_FXM_HDR_SIZE, NC_FXM_HDR_SIZE, &m) == NC_FXM_ERR_CMDS,
          "有指令但 cmds_off=0 被拒");
    s.cmds_off = 48;
    build(buf, s);
    CHECK(nc_fxm_parse(buf, NC_FXM_HDR_SIZE, 48, &m) == NC_FXM_ERR_CMDS,
          "指令表超出檔案尾端被拒");
    CHECK(nc_fxm_parse(buf, NC_FXM_HDR_SIZE, 52, &m) == NC_FXM_OK,
          "剛好放得下就收");

    case_begin("指令表：讀得回來");
    uint8_t file[52];
    memset(file, 0, sizeof(file));
    s = good_spec();
    s.ncmds = 2;
    s.cmds_off = 48;
    build(file, s);
    put_u16(file + 48, 0x3101);
    put_u16(file + 50, 0x3102);
    CHECK(nc_fxm_parse(file, NC_FXM_HDR_SIZE, sizeof(file), &m) == NC_FXM_OK, "解析成功");
    uint16_t cmd = 0;
    CHECK(nc_fxm_cmd_at(file, sizeof(file), &m, 0, &cmd) && cmd == 0x3101, "第 0 條 = 0x3101");
    CHECK(nc_fxm_cmd_at(file, sizeof(file), &m, 1, &cmd) && cmd == 0x3102, "第 1 條 = 0x3102");
    CHECK(!nc_fxm_cmd_at(file, sizeof(file), &m, 2, &cmd), "越界回 false");

    printf("\n%s（%d 個失敗）\n", s_fail ? "FAILED" : "ALL PASS", s_fail);
    return s_fail ? 1 : 0;
}
