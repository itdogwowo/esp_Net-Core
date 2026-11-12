"""C firmware 語法檢查（唔需要 ESP-IDF，亦唔需要板）。

把 `nc_file_action.c` 的 preprocessor 抽走、巨集換成字面值，然後用 pycparser
parse。呢個唔係 IDF build，但可以擋住「少個括號／打錯欄位／漏分號」呢類
會令 build 直接死嘅低級錯誤。

冇裝 pycparser 就 skip（requirements-dev.txt 有）。
"""
import re
from pathlib import Path

import pytest

pycparser = pytest.importorskip("pycparser")
from pycparser import c_parser  # noqa: E402

ROOT = Path(__file__).resolve().parents[3]
SRC = ROOT / "firmware/components/action/file_action/nc_file_action.c"

PRELUDE = r"""
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef long long int64_t;
typedef unsigned long size_t;
typedef _Bool bool;

typedef struct FILE FILE;
typedef struct DIR DIR;
struct dirent { char d_name[256]; };
struct stat { unsigned int st_mode; unsigned long st_size; };

typedef int esp_err_t;

typedef void *SemaphoreHandle_t;

typedef struct { int _op; } psa_hash_operation_t;

typedef unsigned long DWORD;
typedef struct { unsigned csize; unsigned ssize; unsigned n_fatent; } FATFS;

typedef struct cJSON {
    int valueint;
    char *valuestring;
} cJSON;

typedef struct {
    uint16_t addr;
    uint16_t cmd;
    const uint8_t *data;
    uint16_t len;
} nc4_frame_t;

typedef struct nc4_ctx {
    void *decoder;
    esp_err_t (*send)(void *arg, const uint8_t *frame, size_t len);
    void *send_arg;
    const char *source;
} nc4_ctx_t;

typedef void (*nc4_handler_t)(nc4_ctx_t *ctx, const nc4_frame_t *frame);

typedef struct {
    uint16_t cmd;
    nc4_handler_t fn;
    const char *owner;
} nc_action_t;

typedef struct {
    const char *svc;
    const nc_action_t *list;
    size_t count;
} nc_action_table_t;

int nc4_pack_into(uint8_t *out, size_t cap, uint16_t cmd, uint16_t addr,
                  const uint8_t *data, size_t len);
uint16_t nc4_reply_addr(nc4_ctx_t *ctx);
int64_t esp_timer_get_time(void);

const char *nc_store_fs_base(void);
const char *nc_store_fs_drive(void);
bool nc_store_app_ready(void);
const char *nc_store_mode_name(void);
int nc_store_claim_app(void);
int nc_store_release_usb(void);
bool nc_store_sd_on_usb(void);
void nc_boot_request_format(bool on);
int nc_log_set_levels(uint8_t mode);

int f_getfree(const char *drv, DWORD *nclst, FATFS **fs);
"""

REPLACE = {
    "FILE_BEGIN": "0x2001", "FILE_CHUNK": "0x2002", "FILE_END": "0x2003",
    "FILE_ACK": "0x2004", "FILE_QUERY": "0x2005", "FILE_QUERY_RSP": "0x2006",
    "FILE_READ": "0x2007", "FILE_CONFIRM": "0x2008", "FILE_DELETE": "0x2009",
    "FILE_UNDO": "0x200A", "FILE_SCAN": "0x200B", "FILE_ERROR_RSP": "0x2010", "STATUS_SET": "0x1103", "STATUS_ACK": "0x1104",
    "PATH_MAX_LOCAL": "192", "FILE_FULL_MAX": "208", "FILE_CHUNK_MAX": "4096",
    "NC4_MAX_PAYLOAD": "8192", "NC4_HDR_LEN": "9",
    "NC4_CRC_LEN": "4", "NC4_ADDR_BROADCAST": "0xFFFF",
    "PSA_SUCCESS": "0", "PSA_ALG_SHA_256": "1",
    "NC_STORE_SD_MOUNT": '"/sd"',
}


def _preprocessed() -> str:
    text = SRC.read_text(encoding="utf-8")
    text = "\n".join(ln for ln in text.splitlines() if not ln.lstrip().startswith("#"))
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    text = re.sub(r"ESP_LOG[A-Z]\([^;]*;", "", text)
    text = re.sub(r"S_ISREG\([^)]*\)", "1", text)
    text = re.sub(r"S_ISDIR\([^)]*\)", "0", text)
    text = re.sub(r"pdMS_TO_TICKS\(([^)]*)\)", r"(\1)", text)
    for k, v in REPLACE.items():
        text = re.sub(rf"\b{k}\b", v, text)
    text = text.replace("PSA_HASH_OPERATION_INIT", "{0}")
    text = re.sub(
        r"NC_ACTION_TABLE_DEF\([^;]*\);",
        'const nc_action_table_t nc_file_action_table = { "file", s_file_actions, 1 };',
        text,
    )
    return PRELUDE + "\n" + text


def test_nc_file_action_parses():
    parser = c_parser.CParser()
    parser.parse(_preprocessed(), filename=str(SRC))
