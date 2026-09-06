/*
 * sleep_manager.h —— 睡眠管理器的空实现垫片。
 *
 * custom_lcd_display.cc 是从社区移植版 cattei/xiaozhi-zectrix 原样拿过来的
 * （MIT，深圳芯智未来 / 极趣）。它 include 了 "common/sleep_manager.h"，
 * 但**那个文件在社区版仓库里根本不存在** —— 大概是发布时漏掉了。
 * 这里按调用点反推出接口，全部实现成空操作。
 *
 * 两个 API 的语义：
 *   sm_kick(ms, reason)          接下来 ms 毫秒内别进深睡
 *   sm_set_busy(src, busy)       某个子系统正忙/空闲，忙的时候不许睡
 * 都是为了避免墨水屏刷到一半被睡眠打断。
 *
 * 本项目不需要它们：我们定了**一直开唤醒词**（见 CLAUDE.md 硬约束 5 与
 * 2026-09-03 的迁移决策），麦克风常开 + AFE 常跑本来就不能深睡，
 * 所以没有任何东西会在刷屏中途把设备睡掉。
 *
 * **哪天改回「电池时深睡」，这里必须换成真实实现**，否则墨水屏会刷一半
 * 断电，屏上留半张残影 —— 到时候把这段注释一起改掉。
 */

#ifndef ZECTRIX_NOTE4_SLEEP_MANAGER_SHIM_H
#define ZECTRIX_NOTE4_SLEEP_MANAGER_SHIM_H

#include <stdint.h>

/* 调用点只用到 Display 一个来源；其余按社区版可能的取值补齐，
 * 多几个枚举值不花任何代价，少了却会编译失败。 */
enum class SleepBusySrc {
    Display,
    Audio,
    Network,
    Other,
};

static inline void sm_kick(uint32_t keep_awake_ms, const char *reason) {
    (void)keep_awake_ms;
    (void)reason;
}

static inline void sm_set_busy(SleepBusySrc src, bool busy) {
    (void)src;
    (void)busy;
}

#endif /* ZECTRIX_NOTE4_SLEEP_MANAGER_SHIM_H */
