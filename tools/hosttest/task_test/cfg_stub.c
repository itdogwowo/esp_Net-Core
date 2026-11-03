// task_test 的替身：極簡 cJSON ＋ 表驅動的 nc_cfg
//
// 為什麼要這個檔案：
//   `nc_task.c` 現在會讀 /data/config/tasks.json，host 上沒有檔案系統，
//   所以這裡用一張表假裝 config —— 順便讓測試能驗「未設定 = (0,0)」與
//   affinity 搬核。只實作 nc_task.c 真的用到的那幾個 cJSON 函式。
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "cfg_stub.h"
#include "nc_cfg.h"

struct cfg_file {
    cJSON *root;
};

// ───────────────────────── 極簡 cJSON ─────────────────────────

static char *dup_str(const char *s)
{
    const size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p != NULL) {
        memcpy(p, s, n);
    }
    return p;
}

static cJSON *node_new(int type)
{
    cJSON *n = calloc(1, sizeof(*n));
    if (n != NULL) {
        n->type = type;
    }
    return n;
}

static void append_child(cJSON *parent, cJSON *child)
{
    if (parent == NULL || child == NULL) {
        return;
    }
    if (parent->child == NULL) {
        parent->child = child;
        return;
    }
    cJSON *c = parent->child;
    while (c->next != NULL) {
        c = c->next;
    }
    c->next = child;
}

static cJSON *obj_new(void) { return node_new(cJSON_Object); }
static cJSON *arr_new(void) { return node_new(cJSON_Array); }
static cJSON *null_new(void) { return node_new(0); }

static cJSON *num_new(int v)
{
    cJSON *n = node_new(cJSON_Number);
    if (n != NULL) {
        n->valueint = v;
    }
    return n;
}

static cJSON *str_new(const char *s)
{
    cJSON *n = node_new(cJSON_String);
    if (n != NULL) {
        n->valuestring = dup_str(s ? s : "");
    }
    return n;
}

static void obj_add(cJSON *obj, const char *key, cJSON *v)
{
    if (v == NULL) {
        return;
    }
    v->string = dup_str(key);
    append_child(obj, v);
}

static void free_tree(cJSON *n)
{
    while (n != NULL) {
        cJSON *next = n->next;
        free_tree(n->child);
        free(n->string);
        free(n->valuestring);
        free(n);
        n = next;
    }
}

const cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON *object, const char *key)
{
    if (object == NULL || key == NULL || !(object->type & cJSON_Object)) {
        return NULL;
    }
    for (const cJSON *c = object->child; c != NULL; c = c->next) {
        if (c->string != NULL && strcmp(c->string, key) == 0) {
            return c;
        }
    }
    return NULL;
}

const cJSON *cJSON_GetArrayItem(const cJSON *array, int index)
{
    if (array == NULL || index < 0) {
        return NULL;
    }
    int i = 0;
    for (const cJSON *c = array->child; c != NULL; c = c->next, i++) {
        if (i == index) {
            return c;
        }
    }
    return NULL;
}

int cJSON_GetArraySize(const cJSON *array)
{
    int n = 0;
    if (array == NULL) {
        return 0;
    }
    for (const cJSON *c = array->child; c != NULL; c = c->next) {
        n++;
    }
    return n;
}

int cJSON_IsArray(const cJSON *item) { return item != NULL && (item->type & cJSON_Array); }
int cJSON_IsString(const cJSON *item) { return item != NULL && (item->type & cJSON_String); }
int cJSON_IsNumber(const cJSON *item) { return item != NULL && (item->type & cJSON_Number); }

// ───────────────────────── 假 config ─────────────────────────

#define APPENDED_MAX 8
#define APPENDED_LEN 192

static const cfg_stub_task_t *s_tasks;
static int s_ntasks;
static char s_appended[APPENDED_MAX][APPENDED_LEN];
static int s_nappended;

void cfg_stub_set(const cfg_stub_task_t *tasks, int n)
{
    s_tasks = tasks;
    s_ntasks = (tasks != NULL) ? n : 0;
    s_nappended = 0;
}

void cfg_stub_clear(void) { cfg_stub_set(NULL, 0); }

int cfg_stub_appended_count(void) { return s_nappended; }

const char *cfg_stub_appended(int i)
{
    return (i >= 0 && i < s_nappended) ? s_appended[i] : NULL;
}

cfg_file_t *nc_cfg_open(const char *layer)
{
    if (layer == NULL || strcmp(layer, "tasks") != 0) {
        return NULL;                    // 其他層：當作缺檔
    }
    cfg_file_t *cfg = calloc(1, sizeof(*cfg));
    cJSON *root = obj_new();
    cJSON *arr = arr_new();
    if (cfg == NULL || root == NULL || arr == NULL) {
        free(cfg);
        free_tree(root);
        free_tree(arr);
        return NULL;
    }
    obj_add(root, "tasks", arr);

    for (int i = 0; i < s_ntasks; i++) {
        cJSON *it = obj_new();
        if (it == NULL) {
            break;
        }
        obj_add(it, "name", str_new(s_tasks[i].name));
        if (s_tasks[i].has_level) {
            obj_add(it, "level", num_new(s_tasks[i].level));
        }
        if (s_tasks[i].has_aff) {
            cJSON *af = arr_new();
            if (af != NULL) {
                for (int k = 0; k < 2; k++) {
                    append_child(af, (s_tasks[i].aff[k] < 0) ? null_new()
                                                             : num_new(s_tasks[i].aff[k]));
                }
                obj_add(it, "affinity", af);
            }
        }
        append_child(arr, it);
    }

    cfg->root = root;
    return cfg;
}

void nc_cfg_close(cfg_file_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    free_tree(cfg->root);
    free(cfg);
}

const cJSON *nc_cfg_root(const cfg_file_t *cfg) { return (cfg != NULL) ? cfg->root : NULL; }

const char *nc_cfg_str(const cJSON *root, const char *key, const char *def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, key);
    return (v != NULL && v->valuestring != NULL) ? v->valuestring : def;
}

int nc_cfg_int(const cJSON *root, const char *key, int def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsNumber(v) ? v->valueint : def;
}

bool nc_cfg_set_string(const char *layer, const char *key, const char *value)
{
    (void)layer;
    (void)key;
    (void)value;
    return false;                       // host test 不寫檔
}

bool nc_cfg_append_obj(const char *layer, const char *array_key, const char *obj_json)
{
    if (layer == NULL || array_key == NULL || obj_json == NULL) {
        return false;
    }
    if (strcmp(layer, "tasks") != 0 || strcmp(array_key, "tasks") != 0) {
        return false;
    }
    if (s_nappended >= APPENDED_MAX) {
        return false;
    }
    const size_t n = strlen(obj_json);
    if (n >= sizeof(s_appended[0])) {
        return false;
    }
    memcpy(s_appended[s_nappended], obj_json, n + 1);
    s_nappended++;
    return true;
}
