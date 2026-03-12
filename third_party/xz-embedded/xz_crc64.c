/*
 * XZ decompressor - CRC64
 *
 * Based on xz-embedded by Lasse Collin (public domain).
 * CRC64 with polynomial 0xC96C5795D7870F42 (ECMA-182).
 */

#include "xz_private.h"

uint64_t xz_crc64_table[256];

void xz_crc64_init(void)
{
    static int done = 0;
    uint64_t i, j, r;

    if (done)
        return;

    for (i = 0; i < 256; ++i) {
        r = i;
        for (j = 0; j < 8; ++j)
            r = (r >> 1) ^ (UINT64_C(0xC96C5795D7870F42) & ~((r & 1) - 1));
        xz_crc64_table[i] = r;
    }

    done = 1;
}

uint64_t xz_crc64(const uint8_t *buf, size_t size, uint64_t crc)
{
    crc = ~crc;

    while (size > 0) {
        crc = xz_crc64_table[(crc ^ *buf) & 0xFF] ^ (crc >> 8);
        ++buf;
        --size;
    }

    return ~crc;
}
