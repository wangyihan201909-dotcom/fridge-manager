/**
 * fridge_app.h —— 冰箱管家：小智固件里的一个页面。
 *
 * 设计（见 CLAUDE.md 2026-09-04 条目）：
 *   不说话的时候屏上就是冰箱，说话的时候小智接管画对话，聊完自动回冰箱。
 *   这东西本质是一张贴在冰箱上的便利贴，顺便会说话 —— 默认态不该让人按键。
 *
 * 版面**全部由云端渲染**（硬约束 3）：/device/sync 下发 400×300 1bpp 位图，
 * 固件只负责解 RLE 再灌进屏幕，不做任何排版。唯一的例外是右下角的
 * 状态条带（时间 + 电量），由固件自绘并**只局刷那一块** —— 因为位图只在
 * rev 变化时才下发，云端画的钟必然是停的。
 */

#ifndef FRIDGE_APP_H
#define FRIDGE_APP_H

class CustomLcdDisplay;

/**
 * 启动冰箱管家后台任务。在 board 构造完显示之后调用一次。
 * 任务自己等网络、对时、绑定、轮询，不阻塞调用方。
 */
void fridge_app_start(CustomLcdDisplay *display);

/**
 * 在「冰箱页」与「小智页」之间切换（上键单击）。
 *
 * 两套 UI 写的是同一个帧缓冲，谁后写谁赢 —— 所以必须显式切换，
 * 靠「谁刷得勤」是稳不住的。冰箱页期间 LVGL 被挡住不落笔。
 */
void fridge_app_toggle_page(void);

#endif /* FRIDGE_APP_H */
