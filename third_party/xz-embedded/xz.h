/*
 * XZ decompressor - Public API header
 *
 * Based on xz-embedded by Lasse Collin (public domain).
 * Minimal subset for streaming XZ decompression.
 */

#ifndef XZ_H
#define XZ_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * enum xz_mode - Operation mode
 *
 * @XZ_SINGLE:    Single-call mode. The caller provides the full input and
 *                output buffers in one call.
 * @XZ_PREALLOC:  Multi-call mode with preallocated dictionary buffer.
 * @XZ_DYNALLOC:  Multi-call mode with dynamically allocated dictionary.
 */
enum xz_mode {
    XZ_SINGLE,
    XZ_PREALLOC,
    XZ_DYNALLOC
};

/**
 * enum xz_ret - Return codes
 *
 * @XZ_OK:                 Everything is OK so far. More input or output
 *                         space is needed to continue.
 * @XZ_STREAM_END:         Operation finished successfully.
 * @XZ_UNSUPPORTED_CHECK:  Integrity check type is not supported. Decoding
 *                         is still possible by simply ignoring the check.
 * @XZ_MEM_ERROR:          Allocating memory failed.
 * @XZ_MEMLIMIT_ERROR:     A bigger dictionary would be needed than allowed
 *                         by dict_max in xz_dec_init().
 * @XZ_FORMAT_ERROR:       File format was not recognized (wrong magic bytes).
 * @XZ_OPTIONS_ERROR:      This implementation doesn't support the requested
 *                         compression options. In the decoder this means
 *                         unsupported header flags.
 * @XZ_DATA_ERROR:         Compressed data is corrupt.
 * @XZ_BUF_ERROR:          Cannot make any progress. Details depend on
 *                         function being called.
 */
enum xz_ret {
    XZ_OK,
    XZ_STREAM_END,
    XZ_UNSUPPORTED_CHECK,
    XZ_MEM_ERROR,
    XZ_MEMLIMIT_ERROR,
    XZ_FORMAT_ERROR,
    XZ_OPTIONS_ERROR,
    XZ_DATA_ERROR,
    XZ_BUF_ERROR
};

/**
 * struct xz_buf - Passing input and output buffers to XZ code
 *
 * @in:         Beginning of the input buffer.
 * @in_pos:     Current position in the input buffer. This must not exceed
 *              in_size.
 * @in_size:    Size of the input buffer.
 * @out:        Beginning of the output buffer.
 * @out_pos:    Current position in the output buffer. This must not exceed
 *              out_size.
 * @out_size:   Size of the output buffer.
 */
struct xz_buf {
    const uint8_t *in;
    size_t in_pos;
    size_t in_size;

    uint8_t *out;
    size_t out_pos;
    size_t out_size;
};

/* Opaque decoder state */
struct xz_dec;

/**
 * xz_crc32_init() - Initialize the CRC32 lookup table.
 * Must be called before any CRC32 use (including xz_dec_run).
 */
void xz_crc32_init(void);

/**
 * xz_crc64_init() - Initialize the CRC64 lookup table.
 * Must be called if the stream uses CRC64 checks.
 */
void xz_crc64_init(void);

/**
 * xz_dec_init() - Allocate and initialize a XZ decoder state.
 * @mode:       XZ_SINGLE, XZ_PREALLOC, or XZ_DYNALLOC.
 * @dict_max:   Maximum allowed dictionary size. Use 0 for default.
 *
 * Returns NULL on allocation failure.
 */
struct xz_dec *xz_dec_init(enum xz_mode mode, uint32_t dict_max);

/**
 * xz_dec_run() - Run the XZ decoder.
 * @s:  Decoder state allocated with xz_dec_init().
 * @b:  Input/output buffer pointers.
 *
 * See enum xz_ret for possible return values.
 */
enum xz_ret xz_dec_run(struct xz_dec *s, struct xz_buf *b);

/**
 * xz_dec_reset() - Reset an already allocated decoder state.
 * @s:  Decoder state allocated with xz_dec_init().
 *
 * This allows reusing the decoder state for decoding another stream.
 */
void xz_dec_reset(struct xz_dec *s);

/**
 * xz_dec_end() - Free the decoder state.
 * @s:  Decoder state allocated with xz_dec_init(). Passing NULL is safe.
 */
void xz_dec_end(struct xz_dec *s);

#ifdef __cplusplus
}
#endif

#endif /* XZ_H */
