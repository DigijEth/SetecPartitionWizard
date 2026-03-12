/*
 * XZ decompressor - CRC32
 *
 * Based on xz-embedded by Lasse Collin (public domain).
 * Standard CRC32 with polynomial 0xEDB88320 (bit-reversed 0x04C11DB7).
 */

#include "xz_private.h"

uint32_t xz_crc32_table[256];

void xz_crc32_init(void)
{
    static int done = 0;
    uint32_t i, j, r;

    if (done)
        return;

    for (i = 0; i < 256; ++i) {
        r = i;
        for (j = 0; j < 8; ++j)
            r = (r >> 1) ^ (0xEDB88320 & ~((r & 1) - 1));
        xz_crc32_table[i] = r;
    }

    done = 1;
}

uint32_t xz_crc32(const uint8_t *buf, size_t size, uint32_t crc)
{
    crc = ~crc;

    while (size > 0) {
        crc = xz_crc32_table[(crc ^ *buf) & 0xFF] ^ (crc >> 8);
        ++buf;
        --size;
    }

    return ~crc;
}
