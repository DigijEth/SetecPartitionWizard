/*
 * XZ decompressor - Private/internal header
 *
 * Based on xz-embedded by Lasse Collin (public domain).
 */

#ifndef XZ_PRIVATE_H
#define XZ_PRIVATE_H

#include "xz.h"
#include <stdlib.h>
#include <string.h>

/* Six-byte magic at the start of every .xz stream: 0xFD, '7', 'z', 'X', 'Z', 0x00 */
#define XZ_HEADER_MAGIC "\3757zXZ\0"
#define XZ_HEADER_MAGIC_SIZE 6

/* Two-byte magic in stream footer: 'Y', 'Z' */
#define XZ_FOOTER_MAGIC "YZ"
#define XZ_FOOTER_MAGIC_SIZE 2

#ifndef min_t
#define min_t(type, a, b) ((type)(a) < (type)(b) ? (type)(a) : (type)(b))
#endif

#ifndef max_t
#define max_t(type, a, b) ((type)(a) > (type)(b) ? (type)(a) : (type)(b))
#endif

/* Get an unaligned 32-bit little-endian value */
static inline uint32_t get_le32(const uint8_t *buf)
{
    return (uint32_t)buf[0]
         | ((uint32_t)buf[1] << 8)
         | ((uint32_t)buf[2] << 16)
         | ((uint32_t)buf[3] << 24);
}

/* CRC lookup tables */
extern uint32_t xz_crc32_table[256];
extern uint64_t xz_crc64_table[256];

/* CRC functions */
uint32_t xz_crc32(const uint8_t *buf, size_t size, uint32_t crc);
uint64_t xz_crc64(const uint8_t *buf, size_t size, uint64_t crc);

/* LZMA2 decoder */
struct xz_dec_lzma2;

struct xz_dec_lzma2 *xz_dec_lzma2_create(enum xz_mode mode, uint32_t dict_max);
enum xz_ret xz_dec_lzma2_run(struct xz_dec_lzma2 *s, struct xz_buf *b);
enum xz_ret xz_dec_lzma2_reset(struct xz_dec_lzma2 *s, uint8_t props);
void xz_dec_lzma2_end(struct xz_dec_lzma2 *s);

#endif /* XZ_PRIVATE_H */
