/**
 * rle.c —— RLE 解码实现。
 *
 * 固件侧只有解码（云端编码、固件解码，单向）。解码失败返回 -1，
 * 调用方保留旧位图 —— 宁可不刷，不刷一张花的。
 */

#include "rle.h"

int rle_decode(const unsigned char *in, size_t in_len,
               unsigned char *out, size_t out_cap) {
    if (in == NULL || out == NULL || (in_len & 1) != 0) return -1;

    size_t written = 0;
    for (size_t i = 0; i < in_len; i += 2) {
        unsigned int n = in[i];      /* 1..255 */
        unsigned char v = in[i + 1];
        if (n == 0) return -1;       /* n=0 不是合法编码 */
        if (written + n > out_cap) return -1;
        for (unsigned int k = 0; k < n; k++) out[written++] = v;
    }
    return (int)written;
}
