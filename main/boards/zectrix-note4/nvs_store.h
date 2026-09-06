/**
 * nvs_store.h —— NVS 存储布局（docs/spec-firmware.md「存储布局」）。
 *
 * 命名空间：
 *   wifi   ssid / pass            Wi-Fi 凭据
 *   bind   device_secret / household_id / household_name / device_id
 *   sync   local_rev / screen_hash / poll_interval
 *   queue  op_<n>                 离线操作队列（带幂等 opId）
 *   cfg    speak_hour_am / speak_hour_pm  播报时间
 *
 * 全存 NVS 是为了「刷机保留数据」：极趣平台刷写工具的免重新配网选项
 * 备份并回填 NVS 分区 —— 升级固件后 deviceSecret 与 Wi-Fi 都在。
 */

#ifndef NVS_STORE_H
#define NVS_STORE_H

#include <stddef.h>   /* size_t */
#include <stdint.h>
#include <stdbool.h>

bool nvs_store_init(void);

/* ---------------- wifi ---------------- */
bool nvs_wifi_set(const char *ssid, const char *pass);
bool nvs_wifi_get(char *ssid_out, size_t ssid_len, char *pass_out, size_t pass_len);
bool nvs_wifi_has(void);

/* ---------------- bind ---------------- */
bool nvs_bind_set(const char *device_id, const char *secret,
                  const char *household_id, const char *household_name);
bool nvs_bind_get(char *device_id, size_t id_len,
                  char *secret, size_t secret_len,
                  char *household_id, size_t hh_len);
bool nvs_bind_has(void);          /* 有 secret 才算已绑定 */
bool nvs_bind_clear(void);        /* 解绑（设备端清自身） */

/* ---------------- sync ---------------- */
bool nvs_sync_set_rev(int64_t rev);
int64_t nvs_sync_get_rev(void);
bool nvs_sync_set_screen_hash(const char *hash);
bool nvs_sync_get_screen_hash(char *out, size_t out_len);
bool nvs_sync_set_poll_interval(int32_t sec);
/** 本机时钟是否由我们自己对过。见 net.c 的 sync_clock —— 原厂固件留下的
 *  时钟看着「合理」但差一个时区，不能拿合理性当已对时的依据。 */
bool nvs_sync_set_clock_ok(bool ok);
bool nvs_sync_get_clock_ok(void);

/** 连续鉴权失败次数。攒够阈值才清绑定，单次 401 不算数（可能只是时钟偏了） */
bool nvs_sync_set_auth_fail(int n);
int nvs_sync_get_auth_fail(void);
int32_t nvs_sync_get_poll_interval(void);

/* ---------------- cfg ---------------- */
bool nvs_cfg_set_speak_hours(int am_hour, int pm_hour);
bool nvs_cfg_get_speak_hours(int *am_hour, int *pm_hour);

/* ---------------- queue（离线操作队列，先进先出重放） ---------------- */
bool nvs_queue_push(const char *op_json);
int nvs_queue_count(void);
bool nvs_queue_peek(char *out, size_t out_len);   /* 队首，不删 */
bool nvs_queue_pop(void);                          /* 删队首 */

#endif /* NVS_STORE_H */
