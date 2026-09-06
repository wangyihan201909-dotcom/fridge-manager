#include <string.h>
#include "framebuf.h"

void framebuf_clear(framebuf_t *fb) {
    memset(fb->data, 0, sizeof(fb->data));
}

void framebuf_set(framebuf_t *fb, int x, int y, int ink) {
    if (x < 0 || y < 0 || x >= PROTO_SCREEN_W || y >= PROTO_SCREEN_H) return;
    size_t i = ((size_t)y * PROTO_SCREEN_W + x) >> 3;
    uint8_t mask = 0x80 >> (x & 7);
    if (ink) fb->data[i] |= mask;
    else fb->data[i] &= (uint8_t)~mask;
}

int framebuf_get(const framebuf_t *fb, int x, int y) {
    if (x < 0 || y < 0 || x >= PROTO_SCREEN_W || y >= PROTO_SCREEN_H) return 0;
    size_t i = ((size_t)y * PROTO_SCREEN_W + x) >> 3;
    return (fb->data[i] & (0x80 >> (x & 7))) != 0;
}
