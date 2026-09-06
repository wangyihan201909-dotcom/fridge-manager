/**
 * rle.h —— 屏幕位图 RLE 解码（与云端 screen.js 的 rleEncode 对齐）。
 */

#ifndef RLE_H
#define RLE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 解码 RLE 字节流。
 *
 * 编码（spec-api.md「屏幕位图编码」）：字节流 = 若干 (n, value) 对，
 * n ∈ [1,255] 表示连续 n 个相同字节 value。
 *
 * @param in     RLE 字节流
 * @param in_len 输入长度（必须为偶数）
 * @param out    输出缓冲（至少 PROTO_SCREEN_BYTES 字节）
 * @param out_cap 输出容量
 * @returns 解码出的字节数；输入不合法或超出容量返回 -1
 */
int rle_decode(const unsigned char *in, size_t in_len,
               unsigned char *out, size_t out_cap);


#ifdef __cplusplus
}
#endif

#endif /* RLE_H */
