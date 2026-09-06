/**
 * device.h —— 五个设备端点的客户端（sync / op / ack / voice / bind）。
 *
 * 协议细节见 docs/spec-api.md（已冻结）与本目录 README 的「关键对齐点」。
 * 状态机（app_main.c）只调用这里，不直接碰 HTTP/HMAC。
 */

#ifndef DEVICE_H
#define DEVICE_H

#include <stdbool.h>
#include <stdint.h>
#include "framebuf.h"

/** sync 的返回（200 有内容；304 时 changed=false，其余字段无意义） */
/** 单次 sync 最多接多少条库存。与云端 index.js 的 MAX_ITEMS 对齐。 */
#define MAX_SYNC_ITEMS 40

/** 一条库存。只留语音工具真正要用的字段 —— 设备不是账本。 */
typedef struct {
    char id[40];
    char name[48];
    int  qty;
    char unit[12];
    int64_t expire_at;
    int64_t created_at;
} sync_item_t;

typedef struct {
    bool changed;              /* false = 云端回 304，什么都不许刷 */
    int64_t rev;
    int next_poll_sec;
    int64_t server_time;
    bool has_screen;
    char screen_hash[45];
    framebuf_t framebuf;       /* RLE 解码后的 1bpp 位图 */
    struct {
        char item_id[48];
        char level[8];
        char text[160];
    } speak[3];
    int speak_count;

    /* 库存清单。**只在 200 里下发**（rev 变了才有），304 时设备沿用上一份。
     * items_truncated 为真表示云端截断了 —— 语音回答时要如实说「还有更多」，
     * 不能把不完整的清单当成全部念出来。 */
    sync_item_t items[MAX_SYNC_ITEMS];
    int  item_count;
    bool items_truncated;

    /* 近 30 天消费统计。云端只给数字，结论留给模型说。
     * days<=0 表示这次没带（304，或老版本云函数）。 */
    struct {
        int days;
        int eaten;
        int lost;
        char top_lost_name[48];
        int top_lost_count;
    } stats;
} sync_result_t;

/** voice 的返回 */
typedef struct {
    char asr_text[160];
    char intent[16];
    bool executed;
    char phrase_id[16];        /* builtinPhraseId（无则空串） */
    char reply_text[160];      /* 调试/追问文本 */
    bool has_screen;
    framebuf_t framebuf;
} voice_result_t;

/** 设备 ID：由 MAC 派生，首次启动生成并存 NVS（绑定要用） */
const char *device_id(void);

/** 是否已绑定（NVS 里有 secret） */
bool device_is_bound(void);

/**
 * 绑定：绑定码换 deviceSecret（spec-api POST /device/bind）。
 * 成功把 secret/household 写 NVS。
 */
bool device_bind(const char *base, const char *bind_code);

/**
 * 屏幕配对：要一个 6 位数字码，反复调用同一个接口等待用户在小程序里认领。
 *
 * @param out_code   未认领时回填 6 位码（缓冲 ≥8 字节），用来显示在屏上
 * @param out_claimed 认领并绑定成功时置真，此时密钥已落 NVS
 * @returns 请求本身是否成功（网络/HTTP 层面）
 *
 * 同一台设备重复调用会拿到**同一个码** —— 否则屏上的数字每次轮询都变，
 * 用户根本来不及输入。
 */
bool device_pair(const char *base, char *out_code, size_t out_len, bool *out_claimed);

/**
 * sync（GET /device/sync）：rev 比对，304 只收 nextPollSec 不刷屏。
 * @returns true=成功（200 或 304），false=网络/解析失败（调用方按离线处理）
 */
bool device_sync(const char *base, sync_result_t *out, int battery_pct, const char *power);

/**
 * op（POST /device/op）：删除/吃掉。网络失败进 NVS 离线队列，
 * 下次 device_flush_queue 按序重放（opId 幂等，重放安全）。
 */
bool device_op(const char *base, const char *op, const char *item_id);

/**
 * 语音录入。name 是食材名，qty/unit 可为 0/NULL（云端默认 1 份）。
 * 保质期由**云端**按知识库先验算 —— 设备不带那份知识库，也不该带：
 * 同一样东西从手机加和用嘴加必须落成同一个结果。
 */
bool device_op_add(const char *base, const char *name, int qty, const char *unit);

/**
 * 一次把一批名字加进购物清单。names 用逗号/顿号/空格分隔都行，云端都认。
 * 已在清单里的同名待购项会被跳过（云端去重），通过 out_skipped 回报。
 * 一次调用加一批，而不是让模型连调三次 —— 三次往返在语音里能听出停顿。
 */
bool device_op_shop(const char *base, const char *names, int *out_added, int *out_skipped);

/**
 * 把购物清单里「已勾选买到、但还没录进冰箱」的那批一次性入库。
 * 不需要参数 —— 入库哪些由数据决定（status=bought 且 boughtItemId 为空），
 * 不是用户说出来的。names 缓冲用来回带入库了什么，好念给用户听。
 */
bool device_op_stock(const char *base, char *out_names, size_t out_len, int *out_count);
bool device_flush_queue(const char *base);
int device_queue_len(void);

/** ack（POST /device/ack）：播报完成回写 */
bool device_ack(const char *base, const char *item_id, const char *level);

/**
 * voice（POST /device/voice）：音频传 base64 纯文本（网关会改写裸二进制）。
 * @param audio_b64 16kHz/16bit/mono PCM 的 base64，上限 6s（约 192KB 原始）
 */
bool device_voice(const char *base, const char *audio_b64, voice_result_t *out);

#endif /* DEVICE_H */
