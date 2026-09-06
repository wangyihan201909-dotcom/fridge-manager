/**
 * device.c —— 五端点客户端实现。
 *
 * 与云端 cloudfunctions/device/index.js 的契约逐字段对齐。
 * 注意两处最容易错的点：
 *   1. 签名串 = ts + method + route + body，route 是端点名（不是 URL 路径）；
 *   2. 304 时绝不能刷屏 —— sync 返回 changed=false，屏幕驱动一行都不许碰。
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include "esp_log.h"
#include "mbedtls/md.h"
#include "mbedtls/base64.h"
#include "cJSON.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "device.h"
#include "proto.h"
#include "rle.h"
#include "net.h"
#include "nvs_store.h"

static const char *TAG = "device";

/** 连续多少次 401 才认定「真的被解绑了」。见 device_sync 的 401 分支。 */
#define AUTH_FAIL_LIMIT 5

#define HMAC_HEX_LEN 65

/* ---------------- 小工具 ---------------- */

/**
 * 真实时间的毫秒时间戳。
 *
 * **不要用 esp_timer_get_time()** —— 它是开机以来的微秒数，冷启动约等于 0。
 * 服务端只接受 ±5 分钟内的 ts，拿开机时长去签名会被一律判过期，
 * 而返回的报错长得像鉴权失败，排查方向会完全跑偏。
 * 系统时钟由 net.c 连上 Wi-Fi 后走 SNTP 对准。
 */
static int64_t now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/**
 * 分段喂入的 HMAC-SHA256，结果与「把四段拼起来再算」完全一致。
 *
 * 之所以不先拼再算：voice 端点的 body 是几百 KB 的 base64 音频，
 * 拼进栈上的固定缓冲就是爆栈。分段喂入不需要任何中间缓冲。
 * （proto_signing_string 保留给主机测试用，那边 body 是短的。）
 */
static void hmac_sha256_hex_parts(const char *key, const char *p1, const char *p2,
                                  const char *p3, const char *p4, char out[HMAC_HEX_LEN]) {
    /* 手写 HMAC-SHA256。
     *
     * mbedTLS 3.6+/TF-PSA 把 mbedtls_md_hmac_* 整组移进了
     * #if defined(MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS)，**已经不是公开 API**，
     * 在 IDF 6 下直接报 implicit declaration。**mbedtls/sha256.h 同样不在
     * 公开路径上**（挪到了 mbedtls/private/），所以也不能退而求其次。
     * 公开的只剩通用散列 mbedtls_md_setup/starts/update/finish（而 md_starts 还在，
     * 所以报错会建议你「did you mean」，指向完全无关的函数）。
     * 规范本身只有几行，自己写反而比追着上游的 API 重组跑更稳。
     *
     *   HMAC(K, m) = H((K' ^ opad) || H((K' ^ ipad) || m))
     *   K' = K 补零到 64 字节；K 超过 64 字节时先做一次 H
     */
    unsigned char k[64] = {0};
    size_t klen = strlen(key);
    if (klen > sizeof(k)) {
        mbedtls_md_context_t kc;
        mbedtls_md_init(&kc);
        mbedtls_md_setup(&kc, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0);
        mbedtls_md_starts(&kc);
        mbedtls_md_update(&kc, (const unsigned char *)key, klen);
        mbedtls_md_finish(&kc, k);
        mbedtls_md_free(&kc);
    } else {
        memcpy(k, key, klen);
    }

    unsigned char ipad[64], opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = (unsigned char)(k[i] ^ 0x36);
        opad[i] = (unsigned char)(k[i] ^ 0x5C);
    }

    /* 内层 H(ipad || 各段)。**分段喂入**，不把签名串拼进中间缓冲 ——
     * voice 端点的 body 是几百 KB 的 base64 音频，拼起来会爆栈（v1 栽过）。 */
    mbedtls_md_context_t in_ctx;
    mbedtls_md_init(&in_ctx);
    mbedtls_md_setup(&in_ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0);  /* 0 = 不用它的 HMAC */
    mbedtls_md_starts(&in_ctx);
    mbedtls_md_update(&in_ctx, ipad, sizeof(ipad));
    const char *parts[4] = { p1, p2, p3, p4 };
    for (int i = 0; i < 4; i++) {
        if (parts[i] && parts[i][0] != '\0') {
            mbedtls_md_update(&in_ctx, (const unsigned char *)parts[i], strlen(parts[i]));
        }
    }
    unsigned char inner[32];
    mbedtls_md_finish(&in_ctx, inner);
    mbedtls_md_free(&in_ctx);

    /* 外层 H(opad || inner) */
    mbedtls_md_context_t out_ctx;
    mbedtls_md_init(&out_ctx);
    mbedtls_md_setup(&out_ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0);
    mbedtls_md_starts(&out_ctx);
    mbedtls_md_update(&out_ctx, opad, sizeof(opad));
    mbedtls_md_update(&out_ctx, inner, sizeof(inner));
    unsigned char digest[32];
    mbedtls_md_finish(&out_ctx, digest);
    mbedtls_md_free(&out_ctx);
    for (int i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", digest[i]);
    out[64] = '\0';
}

/** 拼 URL：base + route + ?query */
static void build_url(char *out, size_t out_len,
                      const char *base, const char *route, const char *query) {
    snprintf(out, out_len, "%s/device%s%s%s", base, route,
             query ? "?" : "", query ? query : "");
}

/**
 * 发起一次带 HMAC 的请求。
 * @returns http_resp_t*（非 2xx 也返回，让调用方处理）；网络失败 NULL
 */
static http_resp_t *authed_request(const char *base, const char *route,
                                   const char *method, const char *body,
                                   const char *query, int timeout_ms) {
    char url[256];
    build_url(url, sizeof(url), base, route, query);

    char ts_str[24];
    snprintf(ts_str, sizeof(ts_str), "%lld", (long long)now_ms());

    char secret[72], device_id_buf[48], hh[48];
    if (!nvs_bind_get(device_id_buf, sizeof(device_id_buf), secret, sizeof(secret),
                      hh, sizeof(hh))) {
        return NULL;
    }


    /* 签名串 = ts + method + route + body，分段喂入不拼缓冲（voice 的 body 极大） */
    char sig[HMAC_HEX_LEN];
    hmac_sha256_hex_parts(secret, ts_str, method, route, body, sig);

    /* 401 时唯一能自查的就是这几个入参。ts 若是个小数字说明时钟没对上。 */
    ESP_LOGI(TAG, "auth: ts=%s method=%s route=%s body=%d字节 id=%s sig=%.8s…",
             ts_str, method, route, body ? (int)strlen(body) : 0, device_id_buf, sig);

    char auth[160];
    proto_auth_header(auth, sizeof(auth), device_id_buf, ts_str, sig);

    return net_https(url, method, body, auth, timeout_ms);
}

const char *device_id(void) {
    /* 与 nvs_bind_get 的 device_id 缓冲同宽 —— 窄一点编译器就按最坏情况
       判定可能截断，-Werror=format-truncation 会直接报错。
       实际内容是 "dev" + 12 位十六进制 = 15 字符，远用不满。 */
    static char id[48];
    if (id[0] != '\0') return id;

    char stored[48], secret_tmp[72], hh_tmp[48];
    if (nvs_bind_get(stored, sizeof(stored), secret_tmp, sizeof(secret_tmp),
                     hh_tmp, sizeof(hh_tmp))
        && stored[0] != '\0') {
        snprintf(id, sizeof(id), "%s", stored);
        return id;
    }

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(id, sizeof(id), "dev%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return id;
}

bool device_is_bound(void) {
    return nvs_bind_has();
}

/* ---------------- bind ---------------- */

bool device_pair(const char *base, char *out_code, size_t out_len, bool *out_claimed) {
    if (out_claimed) *out_claimed = false;
    if (out_code && out_len) out_code[0] = 0;

    char body[192];
    snprintf(body, sizeof(body), "{\"deviceId\":\"%s\",\"fwVersion\":\"%s\"}",
             device_id(), PROTO_FW_VERSION);

    char url[256];
    build_url(url, sizeof(url), base, PROTO_ROUTE_PAIR, NULL);

    /* pair 与 bind 一样免鉴权：设备此刻还没有密钥。 */
    http_resp_t *r = net_https(url, "POST", body, NULL, 10000);
    if (!r) { ESP_LOGW(TAG, "pair: 请求没发出去"); return false; }

    bool ok = false;
    if (r->status != 200) {
        ESP_LOGW(TAG, "pair: HTTP %d，响应 %.*s", r->status,
                 (int)(r->body_len > 160 ? 160 : r->body_len), r->body ? r->body : "");
    } else if (r->body) {
        cJSON *j = cJSON_Parse(r->body);
        if (j) {
            const cJSON *claimed = cJSON_GetObjectItem(j, "claimed");
            if (cJSON_IsTrue(claimed)) {
                const cJSON *secret = cJSON_GetObjectItem(j, "deviceSecret");
                const cJSON *hh = cJSON_GetObjectItem(j, "householdId");
                const cJSON *name = cJSON_GetObjectItem(j, "householdName");
                if (cJSON_IsString(secret) && cJSON_IsString(hh)) {
                    ok = nvs_bind_set(device_id(), secret->valuestring, hh->valuestring,
                                      name && cJSON_IsString(name) ? name->valuestring : "æå®¶");
                    if (out_claimed) *out_claimed = ok;
                    ESP_LOGI(TAG, "pair: 已认领，绑定 %s", ok ? "成功" : "失败（NVS 写入）");
                }
            } else {
                const cJSON *code = cJSON_GetObjectItem(j, "code");
                if (cJSON_IsString(code) && out_code) {
                    snprintf(out_code, out_len, "%s", code->valuestring);
                    ok = true;
                }
            }
            cJSON_Delete(j);
        }
    }
    net_https_free(r);
    return ok;
}

bool device_bind(const char *base, const char *bind_code) {
    char body[256];
    proto_bind_body(body, sizeof(body), bind_code, device_id(), PROTO_FW_VERSION);

    char url[256];
    build_url(url, sizeof(url), base, PROTO_ROUTE_BIND, NULL);

    /* bind 不需要 HMAC：设备还没拿到 secret（spec 约定此端点免鉴权） */
    http_resp_t *r = net_https(url, "POST", body, NULL, 10000);
    if (!r) { ESP_LOGE(TAG, "bind: 请求没发出去（网络/TLS）"); return false; }

    /* 绑定码是一次性的：一次尝试必须把每一步的成败都打出来，
     * 否则每查一层就要烧掉一个码。 */
    bool ok = false;
    if (r->status != 200) {
        ESP_LOGE(TAG, "bind: HTTP %d，响应 %.*s", r->status,
                 (int)(r->body_len > 200 ? 200 : r->body_len), r->body ? r->body : "");
    } else if (!r->body) {
        ESP_LOGE(TAG, "bind: 200 但响应体是空的");
    } else {
        cJSON *j = cJSON_Parse(r->body);
        if (!j) {
            ESP_LOGE(TAG, "bind: 响应不是合法 JSON");
        } else {
            const cJSON *ok_n = cJSON_GetObjectItem(j, "ok");
            const cJSON *secret = cJSON_GetObjectItem(j, "deviceSecret");
            const cJSON *hh = cJSON_GetObjectItem(j, "householdId");
            const cJSON *name = cJSON_GetObjectItem(j, "householdName");
            ESP_LOGI(TAG, "bind: ok=%d secret=%d(%d字符) household=%d",
                     cJSON_IsTrue(ok_n), cJSON_IsString(secret),
                     cJSON_IsString(secret) ? (int)strlen(secret->valuestring) : -1,
                     cJSON_IsString(hh));
            if (cJSON_IsTrue(ok_n) && cJSON_IsString(secret) && cJSON_IsString(hh)) {
                ok = nvs_bind_set(device_id(), secret->valuestring, hh->valuestring,
                                  name && cJSON_IsString(name) ? name->valuestring : "我家");
                ESP_LOGI(TAG, "bind: nvs_bind_set=%d，回读 bound=%d", ok, nvs_bind_has());
            }
            cJSON_Delete(j);
        }
    }
    net_https_free(r);
    return ok;
}

/* ---------------- sync ---------------- */

/** cJSON 取整数，字段缺失/类型不对返回默认值（云端字段不可全信） */
static long jnum(const cJSON *obj, const char *key, long def) {
    const cJSON *n = cJSON_GetObjectItem(obj, key);
    return cJSON_IsNumber(n) ? (long)n->valuedouble : def;
}

static void parse_screen(cJSON *screen_n, sync_result_t *out) {
    const cJSON *data = cJSON_GetObjectItem(screen_n, "data");
    const cJSON *hash = cJSON_GetObjectItem(screen_n, "hash");
    if (!cJSON_IsString(data) || !cJSON_IsString(hash)) return;

    /* base64 → 字节 → RLE 解码 → 帧缓冲 */
    size_t olen = 0;
    unsigned char *raw = NULL;
    if (mbedtls_base64_decode(NULL, 0, &olen,
                              (const unsigned char *)data->valuestring,
                              strlen(data->valuestring)) != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
        return;
    }
    raw = malloc(olen);
    if (!raw) return;
    if (mbedtls_base64_decode(raw, olen, &olen,
                              (const unsigned char *)data->valuestring,
                              strlen(data->valuestring)) != 0) {
        free(raw);
        return;
    }

    int decoded = rle_decode(raw, olen, out->framebuf.data, sizeof(out->framebuf.data));
    free(raw);
    if (decoded != PROTO_SCREEN_BYTES) return;   /* 解码失败：不刷，保留旧位图 */

    out->has_screen = true;
    snprintf(out->screen_hash, sizeof(out->screen_hash), "%s", hash->valuestring);
}

bool device_sync(const char *base, sync_result_t *out, int battery_pct, const char *power) {
    memset(out, 0, sizeof(*out));

    char query[176];   /* 多出来的是 hash= 那 64 个十六进制字符 */
    char cur_hash[48] = "";
    nvs_sync_get_screen_hash(cur_hash, sizeof(cur_hash));
    proto_sync_query(query, sizeof(query), (long)nvs_sync_get_rev(),
                     battery_pct, power, cur_hash);

    http_resp_t *r = authed_request(base, PROTO_ROUTE_SYNC, "GET", NULL, query, 10000);
    if (!r) return false;

    bool ok = true;
    if (r->status == 304) {
        nvs_sync_set_auth_fail(0);
        /* 省电第一原则：304 只更新轮询间隔，屏幕一字节不动 */
        out->changed = false;
        if (r->next_poll[0]) out->next_poll_sec = atoi(r->next_poll);
    } else if (r->status == 200 && r->body) {
        cJSON *j = cJSON_Parse(r->body);
        if (!j) {
            ok = false;
        } else {
            nvs_sync_set_auth_fail(0);
            out->changed = true;
            out->rev = jnum(j, "rev", 0);
            out->next_poll_sec = (int)jnum(j, "nextPollSec", 1800);
            out->server_time = jnum(j, "serverTime", 0);

            cJSON *screen_n = cJSON_GetObjectItem(j, "screen");
            if (cJSON_IsObject(screen_n)) parse_screen(screen_n, out);

            cJSON *st = cJSON_GetObjectItem(j, "stats");
            if (cJSON_IsObject(st)) {
                const cJSON *v;
                v = cJSON_GetObjectItem(st, "days");
                out->stats.days = cJSON_IsNumber(v) ? (int)v->valuedouble : 0;
                v = cJSON_GetObjectItem(st, "eaten");
                out->stats.eaten = cJSON_IsNumber(v) ? (int)v->valuedouble : 0;
                v = cJSON_GetObjectItem(st, "lost");
                out->stats.lost = cJSON_IsNumber(v) ? (int)v->valuedouble : 0;
                v = cJSON_GetObjectItem(st, "topLostCount");
                out->stats.top_lost_count = cJSON_IsNumber(v) ? (int)v->valuedouble : 0;
                v = cJSON_GetObjectItem(st, "topLostName");
                if (cJSON_IsString(v)) {
                    snprintf(out->stats.top_lost_name, sizeof(out->stats.top_lost_name),
                             "%s", v->valuestring);
                }
            }

            cJSON *items = cJSON_GetObjectItem(j, "items");
            if (cJSON_IsArray(items)) {
                int n = cJSON_GetArraySize(items);
                if (n > MAX_SYNC_ITEMS) n = MAX_SYNC_ITEMS;
                for (int i = 0; i < n; i++) {
                    cJSON *e = cJSON_GetArrayItem(items, i);
                    if (!cJSON_IsObject(e)) continue;
                    sync_item_t *it = &out->items[out->item_count];
                    const cJSON *v;
                    v = cJSON_GetObjectItem(e, "id");
                    if (cJSON_IsString(v)) snprintf(it->id, sizeof(it->id), "%s", v->valuestring);
                    v = cJSON_GetObjectItem(e, "name");
                    if (cJSON_IsString(v)) snprintf(it->name, sizeof(it->name), "%s", v->valuestring);
                    v = cJSON_GetObjectItem(e, "unit");
                    if (cJSON_IsString(v)) snprintf(it->unit, sizeof(it->unit), "%s", v->valuestring);
                    v = cJSON_GetObjectItem(e, "qty");
                    it->qty = cJSON_IsNumber(v) ? (int)v->valuedouble : 1;
                    v = cJSON_GetObjectItem(e, "expireAt");
                    it->expire_at = cJSON_IsNumber(v) ? (int64_t)v->valuedouble : 0;
                    v = cJSON_GetObjectItem(e, "createdAt");
                    it->created_at = cJSON_IsNumber(v) ? (int64_t)v->valuedouble : 0;
                    out->item_count++;
                }
            }
            {
                const cJSON *tr = cJSON_GetObjectItem(j, "itemsTruncated");
                out->items_truncated = cJSON_IsTrue(tr);
            }

            cJSON *q = cJSON_GetObjectItem(j, "speakQueue");
            if (cJSON_IsArray(q)) {
                for (int i = 0; i < 3; i++) {
                    cJSON *e = cJSON_GetArrayItem(q, i);
                    if (!e) break;
                    const cJSON *item = cJSON_GetObjectItem(e, "itemId");
                    const cJSON *level = cJSON_GetObjectItem(e, "level");
                    const cJSON *text = cJSON_GetObjectItem(e, "text");
                    if (item && cJSON_IsString(item)) {
                        snprintf(out->speak[i].item_id, sizeof(out->speak[i].item_id),
                                 "%s", item->valuestring);
                    }
                    if (level && cJSON_IsString(level)) {
                        snprintf(out->speak[i].level, sizeof(out->speak[i].level),
                                 "%s", level->valuestring);
                    }
                    if (text && cJSON_IsString(text)) {
                        snprintf(out->speak[i].text, sizeof(out->speak[i].text),
                                 "%s", text->valuestring);
                    }
                    out->speak_count++;
                }
            }
            cJSON_Delete(j);
        }
    } else if (r->status == 401) {
        /* secret 被作废（小程序解绑）：清 NVS，回配网流程。
         * **这是破坏性动作**，刚绑完就撞上它的话表现是「绑了又没绑」，
         * 所以必须吼出来，绝不能静默清掉。 */
        /* **不要一次 401 就清绑定。** 服务端为了不给攻击者调试信息，
         * 签名错和时间戳过期返回的是同一个 unauthorized；一次时钟偏差
         * 就把绑定清掉，等于让设备只能物理重新配网。
         * 攒够连续次数才清：时钟问题会自愈，真解绑则会一直 401。 */
        int fails = nvs_sync_get_auth_fail() + 1;
        nvs_sync_set_auth_fail(fails);
        ESP_LOGE(TAG, "sync: 401 鉴权失败（连续第 %d 次，满 %d 次才清绑定）响应 %.*s",
                 fails, AUTH_FAIL_LIMIT,
                 (int)(r->body_len > 200 ? 200 : r->body_len), r->body ? r->body : "");
        if (fails >= AUTH_FAIL_LIMIT) {
            ESP_LOGE(TAG, "sync: 连续失败达上限，清除本地绑定，回配网流程");
            nvs_bind_clear();
            nvs_sync_set_auth_fail(0);
        }
        ok = false;
    } else {
        ESP_LOGW(TAG, "sync: HTTP %d 未处理，响应 %.*s", r->status,
                 (int)(r->body_len > 200 ? 200 : r->body_len), r->body ? r->body : "");
        ok = false;
    }

    net_https_free(r);
    return ok;
}

/* ---------------- op（含离线队列） ---------------- */

static char *new_op_id(char out[40]) {
    snprintf(out, 40, "op%lld%04x", (long long)now_ms(),
             (unsigned)(esp_random() & 0xFFFF));
    return out;
}

bool device_op_stock(const char *base, char *out_names, size_t out_len, int *out_count) {
    char op_id[40];
    new_op_id(op_id);

    char body[128];
    snprintf(body, sizeof(body), "{\"opId\":\"%s\",\"op\":\"stock\"}", op_id);

    http_resp_t *r = authed_request(base, PROTO_ROUTE_OP, "POST", body, NULL, 15000);
    if (!r) { nvs_queue_push(body); return false; }

    bool ok = (r->status == 200);
    if (out_count) *out_count = 0;
    if (out_names && out_len) out_names[0] = 0;

    if (ok && r->body) {
        cJSON *j = cJSON_Parse(r->body);
        if (j) {
            const cJSON *arr = cJSON_GetObjectItem(j, "stocked");
            if (cJSON_IsArray(arr)) {
                int n = cJSON_GetArraySize(arr);
                if (out_count) *out_count = n;
                /* 名字拼成一串念给用户听。超出缓冲就截断 —— 语音里报十几个
                 * 名字本来也没人听得完，数量才是重点。 */
                size_t used = 0;
                for (int i = 0; i < n && out_names; i++) {
                    const cJSON *e = cJSON_GetArrayItem(arr, i);
                    if (!cJSON_IsString(e)) continue;
                    int w = snprintf(out_names + used, out_len - used,
                                     "%s%s", used ? "ã" : "", e->valuestring);
                    if (w < 0 || (size_t)w >= out_len - used) break;
                    used += w;
                }
            }
            cJSON_Delete(j);
        }
    } else if (!ok) {
        ESP_LOGW(TAG, "stock: HTTP %d，响应 %.*s", r->status,
                 (int)(r->body_len > 160 ? 160 : r->body_len), r->body ? r->body : "");
    }
    net_https_free(r);
    return ok;
}

bool device_op_shop(const char *base, const char *names, int *out_added, int *out_skipped) {
    char op_id[40];
    new_op_id(op_id);

    for (const char *p = names; *p; p++) {
        if (*p == 0x22 || *p == 0x5C) {
            ESP_LOGW(TAG, "shop: 名字里有引号或反斜杠，拒绝");
            return false;
        }
    }

    char body[512];
    snprintf(body, sizeof(body),
             "{\"opId\":\"%s\",\"op\":\"shop\",\"names\":\"%s\"}", op_id, names);

    http_resp_t *r = authed_request(base, PROTO_ROUTE_OP, "POST", body, NULL, 10000);
    if (!r) { nvs_queue_push(body); return false; }

    bool ok = (r->status == 200);
    if (ok && r->body) {
        cJSON *j = cJSON_Parse(r->body);
        if (j) {
            const cJSON *a = cJSON_GetObjectItem(j, "added");
            const cJSON *sk = cJSON_GetObjectItem(j, "skipped");
            if (out_added) *out_added = cJSON_IsArray(a) ? cJSON_GetArraySize(a) : 0;
            if (out_skipped) *out_skipped = cJSON_IsArray(sk) ? cJSON_GetArraySize(sk) : 0;
            cJSON_Delete(j);
        }
    } else if (!ok) {
        ESP_LOGW(TAG, "shop: HTTP %d，响应 %.*s", r->status,
                 (int)(r->body_len > 160 ? 160 : r->body_len), r->body ? r->body : "");
    }
    net_https_free(r);
    return ok;
}

bool device_op_add(const char *base, const char *name, int qty, const char *unit) {
    char op_id[40];
    new_op_id(op_id);

    /* 名字直接进 JSON —— 语音转写出来的中文里不会有引号或反斜杠，
     * 但万一有就会拼出坏 JSON，所以只放行安全字符，其余整条拒掉。
     * 宁可让模型重说一次，也不要发一个畸形请求上去。 */
    for (const char *p = name; *p; p++) {
        if (*p == 0x22 || *p == 0x5C) {
            ESP_LOGW(TAG, "add: 名字里有引号或反斜杠，拒绝");
            return false;
        }
    }

    char body[320];
    snprintf(body, sizeof(body),
             "{\"opId\":\"%s\",\"op\":\"add\",\"name\":\"%s\",\"qty\":%d,\"unit\":\"%s\"}",
             op_id, name, qty > 0 ? qty : 1, (unit && unit[0]) ? unit : "ä»½");

    http_resp_t *r = authed_request(base, PROTO_ROUTE_OP, "POST", body, NULL, 10000);
    if (!r) {
        nvs_queue_push(body);
        return false;
    }
    bool ok = (r->status == 200);
    if (!ok) {
        ESP_LOGW(TAG, "add: HTTP %d，响应 %.*s", r->status,
                 (int)(r->body_len > 160 ? 160 : r->body_len), r->body ? r->body : "");
    }
    net_https_free(r);
    return ok;
}

bool device_op(const char *base, const char *op, const char *item_id) {
    char op_id[40];
    new_op_id(op_id);

    char body[256];
    proto_op_body(body, sizeof(body), op_id, op, item_id);

    http_resp_t *r = authed_request(base, PROTO_ROUTE_OP, "POST", body, NULL, 10000);
    if (!r) {
        /* 离线：进 NVS 队列，联网后按序重放（opId 幂等） */
        nvs_queue_push(body);
        return false;
    }

    bool ok = (r->status == 200);
    net_https_free(r);
    return ok;
}

bool device_flush_queue(const char *base) {
    bool any = false;
    while (nvs_queue_count() > 0) {
        char body[256];
        if (!nvs_queue_peek(body, sizeof(body))) break;

        http_resp_t *r = authed_request(base, PROTO_ROUTE_OP, "POST", body, NULL, 10000);
        if (!r) break;   /* 还连不上，留到下次 */
        bool ok = (r->status == 200);
        net_https_free(r);
        if (!ok) break;  /* 服务端明确拒绝，别把头排一条堵死整个队列 */

        nvs_queue_pop();
        any = true;
    }
    return any;
}

int device_queue_len(void) {
    return nvs_queue_count();
}

/* ---------------- ack ---------------- */

bool device_ack(const char *base, const char *item_id, const char *level) {
    char body[160];
    proto_ack_body(body, sizeof(body), item_id, level);

    http_resp_t *r = authed_request(base, PROTO_ROUTE_ACK, "POST", body, NULL, 10000);
    if (!r) return false;
    bool ok = (r->status == 200);
    net_https_free(r);
    return ok;
}

/* ---------------- voice ---------------- */

bool device_voice(const char *base, const char *audio_b64, voice_result_t *out) {
    memset(out, 0, sizeof(*out));

    http_resp_t *r = authed_request(base, PROTO_ROUTE_VOICE, "POST", audio_b64, NULL, 20000);
    if (!r) return false;

    bool ok = false;
    if (r->status == 200 && r->body) {
        cJSON *j = cJSON_Parse(r->body);
        if (j) {
            const cJSON *asr = cJSON_GetObjectItem(j, "asrText");
            const cJSON *intent = cJSON_GetObjectItem(j, "intent");
            const cJSON *exec = cJSON_GetObjectItem(j, "executed");
            cJSON *reply = cJSON_GetObjectItem(j, "reply");
            if (cJSON_IsString(asr)) snprintf(out->asr_text, sizeof(out->asr_text), "%s", asr->valuestring);
            if (cJSON_IsString(intent)) snprintf(out->intent, sizeof(out->intent), "%s", intent->valuestring);
            out->executed = cJSON_IsTrue(exec);
            if (cJSON_IsObject(reply)) {
                const cJSON *pid = cJSON_GetObjectItem(reply, "builtinPhraseId");
                const cJSON *txt = cJSON_GetObjectItem(reply, "text");
                if (cJSON_IsString(pid)) snprintf(out->phrase_id, sizeof(out->phrase_id), "%s", pid->valuestring);
                if (cJSON_IsString(txt)) snprintf(out->reply_text, sizeof(out->reply_text), "%s", txt->valuestring);
            }
            cJSON *screen_n = cJSON_GetObjectItem(j, "screen");
            if (cJSON_IsObject(screen_n)) {
                sync_result_t tmp;
                memset(&tmp, 0, sizeof(tmp));
                parse_screen(screen_n, &tmp);
                if (tmp.has_screen) {
                    out->has_screen = true;
                    out->framebuf = tmp.framebuf;
                }
            }
            cJSON_Delete(j);
            ok = true;
        }
    }
    net_https_free(r);
    return ok;
}
