/**
 * net.h —— Wi-Fi 连接与 HTTPS 请求。
 *
 * 设备永远是请求发起方（硬约束 2）：全部 HTTPS 轮询，没有长连接、没有 MQTT。
 * 每次业务往返都走「连网 → 请求 → 断开」，除了 ACTIVE/PLUGGED 态保持连接。
 */

#ifndef NET_H
#define NET_H

#include <stddef.h>
#include <stdbool.h>

/** 一次 HTTP 响应的内存态。body 由调用方 net_https_free 释放。 */
typedef struct {
    int status;
    char *body;          /* NULL 终止 */
    size_t body_len;
    char next_poll[16];  /* X-Next-Poll 头（304 时必看） */
} http_resp_t;

/** 连 Wi-Fi（阻塞至成功或超时）。凭据在 NVS，未配网返回 false。 */
bool net_wifi_connected(void);

/** SNTP 对时。HMAC 的 ts 是真实毫秒时间戳，服务端只认 ±5 分钟，
 *  靠开机相对时间必然被判过期，而报错长得像鉴权失败。 */
void net_time_sync(void);

/**
 * HTTPS 请求。
 * @param auth_header 可空；HMAC 头由调用方用 proto_auth_header 拼好
 * @returns 堆上分配的响应（含非 2xx），网络失败返回 NULL
 */
http_resp_t *net_https(const char *url, const char *method,
                       const char *body, const char *auth_header,
                       int timeout_ms);

void net_https_free(http_resp_t *r);

#endif /* NET_H */
