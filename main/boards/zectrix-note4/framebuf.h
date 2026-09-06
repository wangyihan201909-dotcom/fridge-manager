/**
 * framebuf.h —— 1bpp 帧缓冲（400×300）。
 *
 * 行序从上到下，字节内高位在前（bit7 = 最左），1 = 墨点。
 * 屏幕驱动的全刷/局刷都从这份缓冲取数据；固件不做排版，
 * 缓冲内容完全由云端渲染好下发（rle_decode 填充）。
 */

#ifndef FRAMEBUF_H
#define FRAMEBUF_H

#include <stdint.h>
#include "proto.h"

typedef struct {
    uint8_t data[PROTO_SCREEN_BYTES];
} framebuf_t;

/** 清屏（全白） */
void framebuf_clear(framebuf_t *fb);

/** 置单个像素（x 向右，y 向下；越界静默忽略） */
void framebuf_set(framebuf_t *fb, int x, int y, int ink);

/** 读单个像素 */
int framebuf_get(const framebuf_t *fb, int x, int y);

#endif /* FRAMEBUF_H */
