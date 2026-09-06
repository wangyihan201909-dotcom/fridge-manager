/**
 * nvs_store.c —— NVS 读写实现。
 *
 * 队列入队是「先写队尾再涨计数」，出队是「先减计数再删队首」——
 * 断电最坏情况是丢一条/重复一条，opId 幂等让重复也安全。
 */

#include <string.h>
#include <stdio.h>
#include "nvs.h"
#include "nvs_flash.h"   /* nvs_flash_init / nvs_flash_erase 在这里，nvs.h 里没有 */
#include "nvs_store.h"

#define NS_WIFI  "wifi"
#define NS_BIND  "bind"
#define NS_SYNC  "sync"
#define NS_QUEUE "queue"
#define NS_CFG   "cfg"

static bool get_str(const char *ns, const char *key, char *out, size_t out_len, const char *def) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) {
        snprintf(out, out_len, "%s", def);
        return false;
    }
    size_t len = out_len;
    esp_err_t err = nvs_get_str(h, key, out, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        snprintf(out, out_len, "%s", def);
        return false;
    }
    return true;
}

static bool set_str(const char *ns, const char *key, const char *val) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_str(h, key, val);
    if (err == ESP_OK) nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

static bool get_i64(const char *ns, const char *key, int64_t *out, int64_t def) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) { *out = def; return false; }
    esp_err_t err = nvs_get_i64(h, key, out);
    nvs_close(h);
    if (err != ESP_OK) { *out = def; return false; }
    return true;
}

static bool set_i64(const char *ns, const char *key, int64_t val) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_i64(h, key, val);
    if (err == ESP_OK) nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

bool nvs_store_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    return err == ESP_OK;
}

/* ---------------- wifi ---------------- */

bool nvs_wifi_set(const char *ssid, const char *pass) {
    return set_str(NS_WIFI, "ssid", ssid) && set_str(NS_WIFI, "pass", pass);
}

bool nvs_wifi_get(char *ssid_out, size_t ssid_len, char *pass_out, size_t pass_len) {
    return get_str(NS_WIFI, "ssid", ssid_out, ssid_len, "")
        && get_str(NS_WIFI, "pass", pass_out, pass_len, "");
}

bool nvs_wifi_has(void) {
    char ssid[64];
    return get_str(NS_WIFI, "ssid", ssid, sizeof(ssid), "") && ssid[0] != '\0';
}

/* ---------------- bind ---------------- */

bool nvs_bind_set(const char *device_id, const char *secret,
                  const char *household_id, const char *household_name) {
    return set_str(NS_BIND, "device_id", device_id)
        && set_str(NS_BIND, "device_secret", secret)
        && set_str(NS_BIND, "household_id", household_id)
        && set_str(NS_BIND, "household_name", household_name);
}

bool nvs_bind_get(char *device_id, size_t id_len,
                  char *secret, size_t secret_len,
                  char *household_id, size_t hh_len) {
    return get_str(NS_BIND, "device_id", device_id, id_len, "")
        && get_str(NS_BIND, "device_secret", secret, secret_len, "")
        && get_str(NS_BIND, "household_id", household_id, hh_len, "");
}

bool nvs_bind_has(void) {
    char secret[72];
    return get_str(NS_BIND, "device_secret", secret, sizeof(secret), "") && secret[0] != '\0';
}

bool nvs_bind_clear(void) {
    nvs_handle_t h;
    if (nvs_open(NS_BIND, NVS_READWRITE, &h) != ESP_OK) return false;
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
    return true;
}

/* ---------------- sync ---------------- */

bool nvs_sync_set_rev(int64_t rev) { return set_i64(NS_SYNC, "local_rev", rev); }
int64_t nvs_sync_get_rev(void) { int64_t v; get_i64(NS_SYNC, "local_rev", &v, 0); return v; }

bool nvs_sync_set_screen_hash(const char *hash) { return set_str(NS_SYNC, "screen_hash", hash); }
bool nvs_sync_get_screen_hash(char *out, size_t out_len) {
    return get_str(NS_SYNC, "screen_hash", out, out_len, "");
}

bool nvs_sync_set_poll_interval(int32_t sec) { return set_i64(NS_SYNC, "poll_interval", sec); }
int32_t nvs_sync_get_poll_interval(void) {
    int64_t v; get_i64(NS_SYNC, "poll_interval", &v, 1800); return (int32_t)v;
}

bool nvs_sync_set_clock_ok(bool ok) { return set_i64(NS_SYNC, "clock_ok", ok ? 1 : 0); }

bool nvs_sync_get_clock_ok(void) {
    int64_t v;
    get_i64(NS_SYNC, "clock_ok", &v, 0);
    return v != 0;
}

bool nvs_sync_set_auth_fail(int n) { return set_i64(NS_SYNC, "auth_fail", n); }

int nvs_sync_get_auth_fail(void) {
    int64_t v;
    get_i64(NS_SYNC, "auth_fail", &v, 0);
    return (int)v;
}

/* ---------------- cfg ---------------- */

bool nvs_cfg_set_speak_hours(int am_hour, int pm_hour) {
    return set_i64(NS_CFG, "speak_hour_am", am_hour)
        && set_i64(NS_CFG, "speak_hour_pm", pm_hour);
}

bool nvs_cfg_get_speak_hours(int *am_hour, int *pm_hour) {
    int64_t am, pm;
    get_i64(NS_CFG, "speak_hour_am", &am, 8);
    get_i64(NS_CFG, "speak_hour_pm", &pm, 21);
    *am_hour = (int)am;
    *pm_hour = (int)pm;
    return true;
}

/* ---------------- queue ----------------
 *
 * 队列键是 "op_<下标>"。**下标按 int 格式化，不是 int64**：
 * NVS 的键上限是 15 字符 + NUL，"op_" 占 3 位，%lld 最坏要 20 位 ——
 * 编译器按最坏情况判定必然截断，-Werror=format-truncation 直接报错，
 * 而且真截断了会写到错误的键上。离线队列只有几条，int 绰绰有余。
 */

bool nvs_queue_push(const char *op_json) {
    int64_t count;
    get_i64(NS_QUEUE, "count", &count, 0);
    char key[16];
    snprintf(key, sizeof(key), "op_%d", (int)count);
    bool ok = set_str(NS_QUEUE, key, op_json);
    if (ok) set_i64(NS_QUEUE, "count", count + 1);
    return ok;
}

int nvs_queue_count(void) {
    int64_t count;
    get_i64(NS_QUEUE, "count", &count, 0);
    return (int)count;
}

bool nvs_queue_peek(char *out, size_t out_len) {
    int64_t count;
    get_i64(NS_QUEUE, "count", &count, 0);
    if (count <= 0) return false;
    char key[16];
    snprintf(key, sizeof(key), "op_0");
    return get_str(NS_QUEUE, key, out, out_len, "");
}

bool nvs_queue_pop(void) {
    /* 整队列左移一档。V1 队列短（几条），O(n) 无所谓 */
    int64_t count;
    get_i64(NS_QUEUE, "count", &count, 0);
    if (count <= 0) return false;

    nvs_handle_t h;
    if (nvs_open(NS_QUEUE, NVS_READWRITE, &h) != ESP_OK) return false;
    char k_from[16], k_to[16];
    for (int64_t i = 0; i < count - 1; i++) {
        snprintf(k_from, sizeof(k_from), "op_%d", (int)(i + 1));
        snprintf(k_to, sizeof(k_to), "op_%d", (int)i);
        size_t len = 512;
        char buf[512];
        if (nvs_get_str(h, k_from, buf, &len) == ESP_OK) nvs_set_str(h, k_to, buf);
    }
    snprintf(k_from, sizeof(k_from), "op_%d", (int)(count - 1));
    nvs_erase_key(h, k_from);
    nvs_set_i64(h, "count", count - 1);
    nvs_commit(h);
    nvs_close(h);
    return true;
}
