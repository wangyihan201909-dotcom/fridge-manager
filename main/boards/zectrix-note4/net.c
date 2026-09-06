/**
 * net.c —— Wi-Fi 与 HTTPS 实现（esp_netif + esp_wifi + esp_http_client）。
 *
 * TLS 走 ESP-IDF 内置的 Mozilla 根证书包（esp_crt_bundle）。腾讯云的
 * *.app.tcloudbase.com 是公共 CA 签发的，包里就有，不需要自己分发证书。
 * 「跳过证书校验」不是一个可选项 —— esp_http_client 没有证书来源时
 * 握手直接失败，那不叫降级，那叫连不上。
 *
 * SNTP 也在这里：HMAC 的 ts 是**真实毫秒时间戳**，服务端只认 ±5 分钟。
 * 冷启动时系统时钟是 1970 年，不对时的话第一个请求就会被判过期。
 */

#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_netif_sntp.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "net.h"
#include "nvs_store.h"

static const char *TAG = "net";


/* Wi-Fi 由小智的 WifiBoard 负责连接与重连，这里不再自己 init/start ——
 * 两边都调 esp_wifi_init 会直接打架。本文件只保留：
 *   net_wifi_connected()  查 STA 有没有拿到 IP
 *   net_time_sync()       SNTP 对时（HMAC 要真实时间戳）
 *   net_https()           HTTPS 请求
 */

void net_time_sync(void) {
    time_t before = time(NULL);

    /* 两个条件都满足才跳过：**我们自己对过**，而且时钟看着还合理。
     *
     * 只看「时钟合理」是不够的 —— 板子上跑过别家固件时，ESP32 内部 RTC 里
     * 可能留着上一套固件设的时间，而那个时间常常是本地时区而非 UTC。
     * 差 8 小时远超服务端 ±5 分钟的 HMAC 窗口，返回的却只是一个笼统的
     * unauthorized，看起来像签名算错了。内部 RTC 跨软复位不丢，
     * 只有断电才清，所以这个坑能一直跟着你。 */
    /* **每次开机都对时，不再信 clock_ok 标记。**
     *
     * 原来的逻辑是「对过一次 + 时钟看着合理 → 跳过」。这个「陈旧信任」
     * 已经骗过两次：v1 那次是原厂固件留下的北京时间，这次是小智的 ota.cc
     * 把时区偏移加进了系统时钟 —— **时钟是在标记置位之后才被改坏的**，
     * 所以「看着合理」根本不能作为「已对时」的依据。
     *
     * 当初跳过是为了省电（深睡唤醒后不重复对时）。现在一直开唤醒词、
     * 本来就不深睡，这个理由不成立了。SNTP 一次几秒，开机做一次不亏。
     *
     * clock_ok 仍然写，但只当诊断信息用，不再作为跳过的依据。 */
    (void)nvs_sync_get_clock_ok();

    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        2, ESP_SNTP_SERVER_LIST("ntp.aliyun.com", "pool.ntp.org"));
    if (esp_netif_sntp_init(&cfg) != ESP_OK) {
        /* 这条一定要 ERROR：对不上时后面每个签名请求都会 401，
         * 而 401 的报文只说 unauthorized，完全看不出是时钟问题 */
        ESP_LOGE(TAG, "SNTP 起不来 —— 时钟对不上，后面所有签名请求都会 401");
        return;
    }
    esp_err_t w = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(8000));
    esp_netif_sntp_deinit();

    /* 打出前后值。HMAC 的 ts 必须是 UTC epoch —— 差一个时区就是差 8 小时，
     * 远超服务端 ±5 分钟的窗口，而返回的只是一个笼统的 unauthorized。 */
    if (w == ESP_OK) nvs_sync_set_clock_ok(true);
    ESP_LOGI(TAG, "clock: SNTP %s，%lld → %lld（UTC）",
             w == ESP_OK ? "成功" : "超时", (long long)before, (long long)time(NULL));
}

/* 小智管连接，我们只问「现在有没有网」。
 * 用「拿到 IP」而不是「关联上 AP」判断 —— 关联成功但 DHCP 没完成时
 * HTTPS 一样发不出去。 */
bool net_wifi_connected(void) {
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta == NULL) return false;
    esp_netif_ip_info_t ip = {0};
    return esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0;
}

/* ---------------- HTTPS ---------------- */

typedef struct {
    char *buf;
    size_t len;
} body_acc_t;

/** 上层用字符串表达方法（协议里签名串也用字符串），到这里换成枚举 */
static esp_http_client_method_t method_of(const char *m) {
    if (m && strcmp(m, "POST") == 0) return HTTP_METHOD_POST;
    return HTTP_METHOD_GET;
}

/** 响应体上限。带屏幕位图的 sync 约 20~30KB，留足余量又不至于被恶意响应撑爆。 */
#define BODY_MAX (256 * 1024)

/**
 * 显式读完响应体。
 *
 * **不能靠 HTTP_EVENT_ON_DATA 回调。** 那个事件只在 esp_http_client_perform()
 * 的流程里触发；这里用的是 open/write/fetch_headers 这套底层 API，
 * 事件回调一次都不会来，body 会一直是 NULL。
 *
 * 症状极具迷惑性：HTTP 状态码是对的（200），请求也确实打到了服务端，
 * 但每个端点都「解析失败」—— 看起来像协议或 JSON 的问题，
 * 实际上根本没把响应读出来。
 */
static void read_body(esp_http_client_handle_t client, body_acc_t *acc) {
    char chunk[512];
    for (;;) {
        int n = esp_http_client_read(client, chunk, sizeof(chunk));
        if (n <= 0) break;                       /* 0=读完，<0=出错 */
        if (acc->len + (size_t)n > BODY_MAX) break;

        char *grown = realloc(acc->buf, acc->len + (size_t)n + 1);
        if (!grown) break;                       /* 内存不够就留住已读到的部分 */
        acc->buf = grown;
        memcpy(acc->buf + acc->len, chunk, (size_t)n);
        acc->len += (size_t)n;
        acc->buf[acc->len] = '\0';
    }
}

http_resp_t *net_https(const char *url, const char *method,
                       const char *body, const char *auth_header,
                       int timeout_ms) {
    body_acc_t acc = {0};

    esp_http_client_config_t cfg = {
        .url = url,
        .method = method_of(method),
        .timeout_ms = timeout_ms,
        /* 内置 Mozilla 根证书包。不给证书来源的话 TLS 握手直接失败。 */
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (auth_header) esp_http_client_set_header(client, "Authorization", auth_header);

    esp_err_t err = esp_http_client_open(client, body ? (int)strlen(body) : 0);
    if (err == ESP_OK) {
        if (body) esp_http_client_write(client, body, (int)strlen(body));
        esp_http_client_fetch_headers(client);
    }

    http_resp_t *r = calloc(1, sizeof(http_resp_t));
    if (!r) { esp_http_client_cleanup(client); return NULL; }

    if (err == ESP_OK) {
        r->status = esp_http_client_get_status_code(client);
        /* IDF 5 里这个函数是三参数、把值写进出参并返回 esp_err_t */
        char *np = NULL;
        if (esp_http_client_get_header(client, "X-Next-Poll", &np) == ESP_OK && np) {
            snprintf(r->next_poll, sizeof(r->next_poll), "%s", np);
        }
        read_body(client, &acc);
        r->body = acc.buf;
        r->body_len = acc.len;
        ESP_LOGI(TAG, "%s %s -> %d (%u 字节)", method ? method : "GET", url,
                 r->status, (unsigned)acc.len);
    } else {
        free(acc.buf);
        free(r);
        r = NULL;
    }

    esp_http_client_cleanup(client);
    return r;
}

void net_https_free(http_resp_t *r) {
    if (!r) return;
    free(r->body);
    free(r);
}
