/**
 * proto.c —— 协议纯逻辑实现。与 cloudfunctions/device/auth.js 逐条对齐：
 * 签名串、端点归一、Authorization 头、各端点请求体。
 */

#include <string.h>
#include <stdio.h>
#include "proto.h"

const char *proto_route_of(const char *path) {
    static const char *ROUTES[] = {
        PROTO_ROUTE_SYNC, PROTO_ROUTE_OP, PROTO_ROUTE_ACK,
        PROTO_ROUTE_VOICE, PROTO_ROUTE_BIND,
    };

    if (path == NULL) return NULL;

    /* 有 /device 前缀就剥掉（"/device".length == 7） */
    const char *p = strstr(path, "/device");
    if (p != NULL) p += 7;
    else p = path;

    /* 剥尾斜杠 */
    size_t len = strlen(p);
    while (len > 0 && p[len - 1] == '/') len--;

    for (size_t i = 0; i < sizeof(ROUTES) / sizeof(ROUTES[0]); i++) {
        if (strlen(ROUTES[i]) == len && strncmp(p, ROUTES[i], len) == 0) {
            return ROUTES[i];
        }
    }
    return NULL;
}

char *proto_signing_string(char *out, const char *ts, const char *method,
                           const char *route, const char *body) {
    /* 严格按 auth.js：ts + method + route + body，无任何分隔符 */
    out[0] = '\0';
    strcat(out, ts);
    strcat(out, method);
    strcat(out, route);
    if (body != NULL) strcat(out, body);
    return out;
}

char *proto_auth_header(char *out, size_t out_len,
                        const char *device_id, const char *ts, const char *sig_hex) {
    snprintf(out, out_len, "HMAC %s:%s:%s", device_id, ts, sig_hex);
    return out;
}

char *proto_sync_query(char *out, size_t out_len, long rev, int battery,
                       const char *power, const char *screen_hash) {
    const char *h = (screen_hash && screen_hash[0]) ? screen_hash : "";
    if (battery >= 0) {
        snprintf(out, out_len, "rev=%ld&battery=%d&power=%s&hash=%s", rev, battery, power, h);
    } else {
        snprintf(out, out_len, "rev=%ld&power=%s&hash=%s", rev, power, h);
    }
    return out;
}

char *proto_op_body(char *out, size_t out_len,
                    const char *op_id, const char *op, const char *item_id) {
    /* opId/op/itemId 都来自固件内部（字母数字下划线），无注入风险 */
    snprintf(out, out_len, "{\"opId\":\"%s\",\"op\":\"%s\",\"itemId\":\"%s\"}",
             op_id, op, item_id);
    return out;
}

char *proto_ack_body(char *out, size_t out_len, const char *item_id, const char *level) {
    snprintf(out, out_len, "{\"itemId\":\"%s\",\"level\":\"%s\"}", item_id, level);
    return out;
}

char *proto_bind_body(char *out, size_t out_len,
                      const char *bind_code, const char *device_id, const char *fw_version) {
    snprintf(out, out_len,
             "{\"bindCode\":\"%s\",\"deviceId\":\"%s\",\"fwVersion\":\"%s\"}",
             bind_code, device_id, fw_version);
    return out;
}
