#include "nc_fx_manifest.h"

#include <string.h>

// 只用固定位移解欄位，不靠 struct packing —— 這樣任何編譯器/平台結果都一樣，
// 也順便讓「格式改版」變成一件看得見的事。
#define OFF_MAGIC      0
#define OFF_VERSION    4
#define OFF_KIND       6
#define OFF_CORE       7
#define OFF_LAYER      8
#define OFF_PRIO       9
#define OFF_FPS        10
#define OFF_PX         12
#define OFF_FLAGS      14
#define OFF_STACK      16
#define OFF_BUDGET     20
#define OFF_MEM        24
#define OFF_CMDS_OFF   28
#define OFF_NCMDS      32
#define OFF_RESERVED   34

static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

const char *nc_fxm_err_name(nc_fxm_err_t err)
{
    switch (err) {
    case NC_FXM_OK:            return "ok";
    case NC_FXM_ERR_MAGIC:     return "bad magic";
    case NC_FXM_ERR_VERSION:   return "unsupported version";
    case NC_FXM_ERR_KIND:      return "bad kind";
    case NC_FXM_ERR_CORE:      return "bad core";
    case NC_FXM_ERR_LAYER:     return "bad layer";
    case NC_FXM_ERR_PRIO:      return "bad priority";
    case NC_FXM_ERR_STACK:     return "bad stack";
    case NC_FXM_ERR_FPS:       return "bad fps";
    case NC_FXM_ERR_PX:        return "bad px";
    case NC_FXM_ERR_MEM:       return "bad mem_bytes";
    case NC_FXM_ERR_BUDGET:    return "bad budget";
    case NC_FXM_ERR_CMDS:      return "bad cmd table";
    default:                   return "?";
    }
}

nc_fxm_err_t nc_fxm_parse(const void *buf, size_t len, uint32_t file_size, nc_fxm_t *out)
{
    if (buf == NULL || out == NULL) {
        return NC_FXM_ERR_MAGIC;
    }
    if (len < NC_FXM_HDR_SIZE) {
        return NC_FXM_ERR_MAGIC;          // 太短 = 根本不是 manifest
    }
    const uint8_t *p = (const uint8_t *)buf;

    if (rd_u32(p + OFF_MAGIC) != NC_FXM_MAGIC) {
        return NC_FXM_ERR_MAGIC;
    }
    if (rd_u16(p + OFF_VERSION) != NC_FXM_VERSION) {
        return NC_FXM_ERR_VERSION;
    }

    const uint8_t kind = p[OFF_KIND];
    // 載入的任務不能自稱 system：system 是「不可卸載、固定順序」的那一層，
    // 由韌體決定，不是由一個可以被換掉的檔案決定。
    if (kind != NC_FXM_KIND_NORMAL && kind != NC_FXM_KIND_EFFECT) {
        return NC_FXM_ERR_KIND;
    }

    const int8_t core = (int8_t)p[OFF_CORE];
    if (core < -1 || core > 1) {
        return NC_FXM_ERR_CORE;
    }

    const uint8_t layer = p[OFF_LAYER];
    if (layer > 7) {
        return NC_FXM_ERR_LAYER;
    }

    const uint8_t prio = p[OFF_PRIO];
    if (prio == 0 || prio > 24) {         // 0 = idle；configMAX_PRIORITIES 預設 25
        return NC_FXM_ERR_PRIO;
    }

    const uint32_t stack = rd_u32(p + OFF_STACK);
    if (stack != 0 && (stack < 1024 || stack > NC_FXM_MAX_STACK)) {
        return NC_FXM_ERR_STACK;
    }

    const uint16_t fps = rd_u16(p + OFF_FPS);
    if (fps > NC_FXM_MAX_FPS) {
        return NC_FXM_ERR_FPS;
    }

    const uint16_t px = rd_u16(p + OFF_PX);
    if (px > NC_FXM_MAX_PX) {
        return NC_FXM_ERR_PX;
    }

    const uint32_t budget = rd_u32(p + OFF_BUDGET);
    if (budget > NC_FXM_MAX_BUDGET_US) {
        return NC_FXM_ERR_BUDGET;
    }

    const uint32_t mem = rd_u32(p + OFF_MEM);
    if (mem > NC_FXM_MAX_MEM) {
        return NC_FXM_ERR_MEM;
    }

    const uint32_t cmds_off = rd_u32(p + OFF_CMDS_OFF);
    const uint16_t ncmds = rd_u16(p + OFF_NCMDS);
    if (ncmds > NC_FXM_MAX_CMDS) {
        return NC_FXM_ERR_CMDS;
    }
    if (ncmds > 0) {
        if (cmds_off == 0) {
            return NC_FXM_ERR_CMDS;
        }
        const uint64_t end = (uint64_t)cmds_off + (uint64_t)ncmds * 2u;
        if (end > file_size) {
            return NC_FXM_ERR_CMDS;       // 指令表跑到檔案外面
        }
    }

    memset(out, 0, sizeof(*out));
    out->kind = kind;
    out->core = core;
    out->layer = layer;
    out->prio = prio;
    out->fps = fps;
    out->px = px;
    out->stack = stack;
    out->budget_us = budget;
    out->mem_bytes = mem;
    out->cmds_off = cmds_off;
    out->ncmds = ncmds;
    out->flags = rd_u16(p + OFF_FLAGS);
    return NC_FXM_OK;
}

bool nc_fxm_cmd_at(const uint8_t *file, size_t file_size, const nc_fxm_t *m,
                   int i, uint16_t *cmd)
{
    if (file == NULL || m == NULL || cmd == NULL || i < 0 || i >= m->ncmds) {
        return false;
    }
    const size_t off = (size_t)m->cmds_off + (size_t)i * 2u;
    if (off + 2 > file_size) {
        return false;
    }
    *cmd = rd_u16(file + off);
    return true;
}
