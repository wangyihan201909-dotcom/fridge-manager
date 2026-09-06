/**
 * proto.h —— 云端协议纯逻辑（无 ESP-IDF 依赖，主机可测）。
 *
 * 与云端 auth.js / index.js 的协议约定一一对应，任何一边改动都要同步另一边：
 *   - 签名串 = ts + method + route + body（route 是端点名，不是完整路径）
 *   - Authorization: HMAC <deviceId>:<ts>:<signature（小写 hex）>
 *   - ts 为毫秒时间戳十进制串，±5 分钟有效
 *
 * 这里只做字符串级构造，HMAC 计算在 device.c 里调 mbedtls —— 纯逻辑部分
 * 因此不需要任何加密库就能在主机上验证（tests/test_proto_rle.c）。
 */

#ifndef PROTO_H
#define PROTO_H

#include <stddef.h>   /* size_t：头文件要能自包含，不能指望调用方先 include string.h */

#ifdef __cplusplus
extern "C" {
#endif

#define PROTO_ROUTE_SYNC   "/sync"
#define PROTO_ROUTE_OP     "/op"
#define PROTO_ROUTE_ACK    "/ack"
#define PROTO_ROUTE_VOICE  "/voice"
#define PROTO_ROUTE_BIND   "/bind"
#define PROTO_ROUTE_PAIR   "/pair"   /* 屏幕配对码，与 bind 一样免鉴权 */

#define PROTO_FW_VERSION   "1.0.0"

/** 屏幕位图解码后的字节数：400×300 1bpp */
#define PROTO_SCREEN_BYTES (400 * 300 / 8)

/** 屏幕宽高（1bpp） */
#define PROTO_SCREEN_W 400
#define PROTO_SCREEN_H 300

/**
 * 把网关给的路径归一成端点名，与云端 auth.js 的 routeOf 同规则：
 * 认 /device/sync、/sync、/sync/、任意前缀 + /device 等形态。
 * 认不出来返回 NULL。
 */
const char *proto_route_of(const char *path);

/**
 * 拼签名串到 out（调用方保证容量 ≥ ts+method+route+body 长度 + 1）。
 * 返回 out。
 */
char *proto_signing_string(char *out, const char *ts, const char *method,
                           const char *route, const char *body);

/** 拼 Authorization 头："HMAC <id>:<ts>:<sig>" */
char *proto_auth_header(char *out, size_t out_len,
                        const char *device_id, const char *ts, const char *sig_hex);

/** 拼 sync 的查询串："rev=..&battery=..&power=.."（battery<0 时不带电池） */
/**
 * sync 的 query。带上设备**自己手里**的 screenHash ——
 * 云端据此判断要不要下发位图。
 *
 * 不能只靠云端记的那个 hash：那个代表「云端上次发过什么」，
 * 设备重刷之后位图丢了，云端并不知道，于是永远不再下发，屏幕停在空白。
 * hash 可为 NULL 或空串（表示「我什么都没有，请给我」）。
 */
char *proto_sync_query(char *out, size_t out_len, long rev, int battery,
                       const char *power, const char *screen_hash);

/** 拼 op 请求体：{"opId":"..","op":"delete|eaten","itemId":".."} */
char *proto_op_body(char *out, size_t out_len,
                    const char *op_id, const char *op, const char *item_id);

/** 拼 ack 请求体：{"itemId":"..","level":"D-1|D0|OD"} */
char *proto_ack_body(char *out, size_t out_len, const char *item_id, const char *level);

/** 拼 bind 请求体：{"bindCode":"..","deviceId":"..","fwVersion":".."} */
char *proto_bind_body(char *out, size_t out_len,
                      const char *bind_code, const char *device_id, const char *fw_version);


#ifdef __cplusplus
}
#endif

#endif /* PROTO_H */
