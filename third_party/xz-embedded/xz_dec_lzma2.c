/*
 * XZ decompressor - LZMA2 decoder
 *
 * Based on xz-embedded by Lasse Collin (public domain).
 *
 * This implements:
 *   - LZMA2 chunk parsing (control bytes, property resets, dictionary resets)
 *   - The LZMA range decoder
 *   - The full LZMA algorithm (literals, matches, short/long reps)
 *   - Dictionary management as a circular buffer
 */

#include "xz_lzma2.h"

/*
 * ============================================================
 * Range decoder
 * ============================================================
 */
struct rc_dec {
    uint32_t range;
    uint32_t code;
    uint32_t init_bytes_left;

    const uint8_t *in;
    size_t in_pos;
    size_t in_limit;
};

static inline void rc_reset(struct rc_dec *rc)
{
    rc->range = 0xFFFFFFFF;
    rc->code = 0;
    rc->init_bytes_left = 5;
}

static inline int rc_read_init(struct rc_dec *rc, struct xz_buf *b)
{
    while (rc->init_bytes_left > 0) {
        if (b->in_pos == b->in_size)
            return 0;
        rc->code = (rc->code << 8) + b->in[b->in_pos++];
        --rc->init_bytes_left;
    }
    return 1;
}

static inline void rc_normalize(struct rc_dec *rc)
{
    if (rc->range < RC_TOP_VALUE) {
        rc->range <<= RC_SHIFT_BITS;
        rc->code = (rc->code << RC_SHIFT_BITS) | rc->in[rc->in_pos++];
    }
}

static inline int rc_bit(struct rc_dec *rc, uint16_t *prob)
{
    uint32_t bound;

    rc_normalize(rc);
    bound = (rc->range >> RC_BIT_MODEL_TOTAL_BITS) * *prob;

    if (rc->code < bound) {
        rc->range = bound;
        *prob += (RC_BIT_MODEL_TOTAL - *prob) >> RC_MOVE_BITS;
        return 0;
    } else {
        rc->range -= bound;
        rc->code -= bound;
        *prob -= *prob >> RC_MOVE_BITS;
        return 1;
    }
}

static inline uint32_t rc_bittree(struct rc_dec *rc, uint16_t *probs,
                                  uint32_t limit)
{
    uint32_t symbol = 1;
    do {
        if (rc_bit(rc, &probs[symbol]))
            symbol = (symbol << 1) + 1;
        else
            symbol <<= 1;
    } while (symbol < limit);
    return symbol;
}

static inline uint32_t rc_bittree_reverse(struct rc_dec *rc, uint16_t *probs,
                                          uint32_t bits)
{
    uint32_t symbol = 1;
    uint32_t i, result = 0;

    for (i = 0; i < bits; ++i) {
        if (rc_bit(rc, &probs[symbol])) {
            symbol = (symbol << 1) + 1;
            result |= 1U << i;
        } else {
            symbol <<= 1;
        }
    }
    return result;
}

static inline uint32_t rc_direct(struct rc_dec *rc, uint32_t count)
{
    uint32_t result = 0;
    do {
        rc_normalize(rc);
        rc->range >>= 1;
        rc->code -= rc->range;
        result = (result << 1) + (rc->code >> 31) + 1;
        rc->code += rc->range & (rc->code >> 31);
    } while (--count > 0);
    return result;
}

/*
 * ============================================================
 * Dictionary (circular buffer)
 * ============================================================
 */
struct dictionary {
    uint8_t *buf;
    size_t pos;
    size_t full;
    size_t limit;
    size_t end;
    uint32_t size;
    enum xz_mode mode;
    uint32_t allocated;
};

static void dict_reset(struct dictionary *dict, struct xz_buf *b)
{
    if (dict->mode == XZ_SINGLE) {
        dict->buf = b->out + b->out_pos;
        dict->end = b->out_size - b->out_pos;
    }
    dict->pos = 0;
    dict->full = 0;
    dict->limit = dict->end;
}

static inline void dict_limit(struct dictionary *dict, size_t out_max)
{
    if (dict->mode == XZ_SINGLE)
        return;
    if (dict->end - dict->pos <= out_max)
        dict->limit = dict->end;
    else
        dict->limit = dict->pos + out_max;
}

static inline int dict_has_space(const struct dictionary *dict)
{
    return dict->pos < dict->limit;
}

static inline uint8_t dict_get(const struct dictionary *dict, uint32_t dist)
{
    size_t offset = dict->pos - dist - 1;
    if (dict->pos <= dist)
        offset += dict->end;
    return dict->buf[offset];
}

static inline void dict_put(struct dictionary *dict, uint8_t byte)
{
    dict->buf[dict->pos++] = byte;
    if (dict->full < dict->pos)
        dict->full = dict->pos;
}

static int dict_repeat(struct dictionary *dict, uint32_t *len, uint32_t dist)
{
    size_t back;
    uint32_t left;

    if (dist >= dict->full || dist >= dict->size)
        return 0;

    left = min_t(uint32_t, (uint32_t)(dict->limit - dict->pos), *len);
    *len -= left;

    back = dict->pos - dist - 1;
    if (dict->pos <= dist)
        back += dict->end;

    while (left > 0) {
        dict->buf[dict->pos++] = dict->buf[back++];
        if (back == dict->end)
            back = 0;
        if (dict->full < dict->pos)
            dict->full = dict->pos;
        --left;
    }

    return 1;
}

static void dict_uncompressed(struct dictionary *dict, struct xz_buf *b,
                              uint32_t *left)
{
    size_t copy_size;

    copy_size = min_t(size_t, b->in_size - b->in_pos,
                      min_t(size_t, *left, dict->limit - dict->pos));

    memcpy(dict->buf + dict->pos, b->in + b->in_pos, copy_size);
    dict->pos += copy_size;
    if (dict->full < dict->pos)
        dict->full = dict->pos;
    b->in_pos += copy_size;
    *left -= (uint32_t)copy_size;
}

static uint32_t dict_flush(struct dictionary *dict, struct xz_buf *b)
{
    size_t copy_size;

    if (dict->mode == XZ_SINGLE) {
        size_t out = dict->pos;
        b->out_pos += out;
        return (uint32_t)out;
    }

    copy_size = min_t(size_t, b->out_size - b->out_pos, dict->pos);
    memcpy(b->out + b->out_pos, dict->buf, copy_size);
    b->out_pos += copy_size;

    if (copy_size < dict->pos)
        memmove(dict->buf, dict->buf + copy_size, dict->pos - copy_size);

    dict->pos -= copy_size;
    return (uint32_t)copy_size;
}

/*
 * ============================================================
 * LZMA decoder
 * ============================================================
 */
struct lzma_dec {
    struct rc_dec rc;

    uint32_t state;
    uint32_t rep0, rep1, rep2, rep3;

    uint32_t lc;
    uint32_t literal_pos_mask;
    uint32_t pos_mask;

    /* Match length decoder */
    uint16_t match_len_choice;
    uint16_t match_len_choice2;
    uint16_t match_len_low[POS_STATES_MAX][LEN_LOW_SYMBOLS];
    uint16_t match_len_mid[POS_STATES_MAX][LEN_MID_SYMBOLS];
    uint16_t match_len_high[LEN_HIGH_SYMBOLS];

    /* Rep length decoder */
    uint16_t rep_len_choice;
    uint16_t rep_len_choice2;
    uint16_t rep_len_low[POS_STATES_MAX][LEN_LOW_SYMBOLS];
    uint16_t rep_len_mid[POS_STATES_MAX][LEN_MID_SYMBOLS];
    uint16_t rep_len_high[LEN_HIGH_SYMBOLS];

    /* Main probabilities */
    uint16_t is_match[STATES][POS_STATES_MAX];
    uint16_t is_rep[STATES];
    uint16_t is_rep0[STATES];
    uint16_t is_rep1[STATES];
    uint16_t is_rep2[STATES];
    uint16_t is_rep0_long[STATES][POS_STATES_MAX];

    uint16_t dist_slot[DIST_STATES][DIST_SLOTS];
    uint16_t dist_special[FULL_DISTANCES - DIST_MODEL_END];
    uint16_t dist_align[ALIGN_SIZE];

    /* Literal probabilities: 3 * 2^lc entries per position coder */
    uint16_t literal[LITERAL_CODERS_MAX][0x300];
};

static void lzma_reset(struct lzma_dec *lzma)
{
    uint16_t *p;
    size_t count, i;

    lzma->state = 0;
    lzma->rep0 = 0;
    lzma->rep1 = 0;
    lzma->rep2 = 0;
    lzma->rep3 = 0;

    /* Initialize all probabilities to RC_BIT_MODEL_TOTAL / 2 */
    p = &lzma->match_len_choice;
    count = (size_t)((uint8_t *)&lzma->literal[LITERAL_CODERS_MAX][0]
                   - (uint8_t *)&lzma->match_len_choice) / sizeof(uint16_t);
    for (i = 0; i < count; ++i)
        p[i] = RC_BIT_MODEL_TOTAL / 2;
}

static void lzma_literal(struct lzma_dec *lzma, struct dictionary *dict)
{
    uint16_t *probs;
    uint32_t symbol;
    uint32_t literal_pos;

    literal_pos = dict->pos & lzma->literal_pos_mask;
    probs = lzma->literal[literal_pos << lzma->lc];

    if (dict->pos > 0 || dict->full > 0) {
        uint8_t prev = dict_get(dict, 0);
        probs = lzma->literal[((literal_pos << lzma->lc)
                + ((uint32_t)prev >> (8 - lzma->lc)))
                & ((LITERAL_CODERS_MAX - 1))];
    }

    if (lzma_state_is_literal(lzma->state)) {
        symbol = rc_bittree(&lzma->rc, probs - 1, 0x100);
    } else {
        uint32_t match_byte = dict_get(dict, lzma->rep0);
        uint32_t offset = 0x100;
        symbol = 1;

        do {
            uint32_t match_bit, bit;

            match_byte <<= 1;
            match_bit = match_byte & offset;
            bit = rc_bit(&lzma->rc, &probs[offset + match_bit + symbol]);
            symbol = (symbol << 1) | bit;
            offset &= ~(match_byte ^ (bit ? ~(uint32_t)0 : 0)) & 0x100;
        } while (symbol < 0x100);
    }

    dict_put(dict, (uint8_t)symbol);
    lzma->state = lzma_state_literal(lzma->state);
}

static uint32_t lzma_len_decode(struct rc_dec *rc,
                                uint16_t *choice, uint16_t *choice2,
                                uint16_t low[][LEN_LOW_SYMBOLS],
                                uint16_t mid[][LEN_MID_SYMBOLS],
                                uint16_t *high,
                                uint32_t pos_state)
{
    if (!rc_bit(rc, choice))
        return rc_bittree(rc, low[pos_state] - 1, LEN_LOW_SYMBOLS)
               - LEN_LOW_SYMBOLS + MATCH_LEN_MIN;

    if (!rc_bit(rc, choice2))
        return rc_bittree(rc, mid[pos_state] - 1, LEN_MID_SYMBOLS)
               - LEN_MID_SYMBOLS + MATCH_LEN_MIN + LEN_LOW_SYMBOLS;

    return rc_bittree(rc, high - 1, LEN_HIGH_SYMBOLS)
           - LEN_HIGH_SYMBOLS + MATCH_LEN_MIN + LEN_LOW_SYMBOLS
           + LEN_MID_SYMBOLS;
}

/*
 * ============================================================
 * LZMA2 decoder
 * ============================================================
 */

enum lzma2_seq {
    SEQ_LZMA2_CONTROL,
    SEQ_LZMA2_UNCOMPRESSED_1,
    SEQ_LZMA2_UNCOMPRESSED_2,
    SEQ_LZMA2_COMPRESSED_0,
    SEQ_LZMA2_COMPRESSED_1,
    SEQ_LZMA2_PROPS,
    SEQ_LZMA2_LZMA_PREPARE,
    SEQ_LZMA2_LZMA_RUN,
    SEQ_LZMA2_COPY
};

struct xz_dec_lzma2 {
    enum lzma2_seq sequence;

    /* Current LZMA2 control byte */
    uint32_t control;

    /* Compressed and uncompressed sizes for the current chunk */
    uint32_t compressed;
    uint32_t uncompressed;

    int need_lzma_init;
    int need_dict_reset;
    int need_props;

    /* Leftover match length from previous call */
    uint32_t match_len;

    struct lzma_dec lzma;
    struct dictionary dict;
};

/*
 * Decode LZMA symbols from the range-coded stream.
 * Returns 1 on success, 0 on error (invalid distance reference).
 */
static int lzma_decode_loop(struct xz_dec_lzma2 *s, struct xz_buf *b,
                            size_t in_avail)
{
    struct lzma_dec *lzma = &s->lzma;
    struct dictionary *dict = &s->dict;
    struct rc_dec *rc = &lzma->rc;
    uint32_t pos_state;

    rc->in = b->in;
    rc->in_pos = b->in_pos;
    rc->in_limit = b->in_pos + in_avail;

    /* Finish leftover match */
    if (s->match_len > 0) {
        if (!dict_repeat(dict, &s->match_len, lzma->rep0)) {
            /* Dictionary full, need to flush output first */
            s->compressed -= (uint32_t)(rc->in_pos - b->in_pos);
            b->in_pos = rc->in_pos;
            return 1;
        }
    }

    while (dict_has_space(dict) && rc->in_pos < rc->in_limit) {
        pos_state = dict->pos & lzma->pos_mask;

        if (!rc_bit(rc, &lzma->is_match[lzma->state][pos_state])) {
            lzma_literal(lzma, dict);
        } else if (rc_bit(rc, &lzma->is_rep[lzma->state])) {
            /* Repeated match */
            uint32_t len;

            if (!rc_bit(rc, &lzma->is_rep0[lzma->state])) {
                if (!rc_bit(rc, &lzma->is_rep0_long[lzma->state][pos_state])) {
                    /* Short rep0 (single byte) */
                    if (dict->full == 0) {
                        s->compressed -= (uint32_t)(rc->in_pos - b->in_pos);
                        b->in_pos = rc->in_pos;
                        return 0;
                    }
                    dict_put(dict, dict_get(dict, lzma->rep0));
                    lzma->state = lzma_state_short_rep(lzma->state);
                    continue;
                }
                /* Long rep0 -- distance stays rep0 */
            } else {
                uint32_t tmp;
                if (!rc_bit(rc, &lzma->is_rep1[lzma->state])) {
                    tmp = lzma->rep1;
                } else if (!rc_bit(rc, &lzma->is_rep2[lzma->state])) {
                    tmp = lzma->rep2;
                    lzma->rep2 = lzma->rep1;
                } else {
                    tmp = lzma->rep3;
                    lzma->rep3 = lzma->rep2;
                    lzma->rep2 = lzma->rep1;
                }
                lzma->rep1 = lzma->rep0;
                lzma->rep0 = tmp;
            }

            len = lzma_len_decode(rc,
                                  &lzma->rep_len_choice,
                                  &lzma->rep_len_choice2,
                                  lzma->rep_len_low,
                                  lzma->rep_len_mid,
                                  lzma->rep_len_high,
                                  pos_state);

            lzma->state = lzma_state_long_rep(lzma->state);

            if (!dict_repeat(dict, &len, lzma->rep0)) {
                s->match_len = len;
                break;
            }
        } else {
            /* Normal match */
            uint32_t len, dist_slot, dist;

            lzma->rep3 = lzma->rep2;
            lzma->rep2 = lzma->rep1;
            lzma->rep1 = lzma->rep0;

            len = lzma_len_decode(rc,
                                  &lzma->match_len_choice,
                                  &lzma->match_len_choice2,
                                  lzma->match_len_low,
                                  lzma->match_len_mid,
                                  lzma->match_len_high,
                                  pos_state);

            dist_slot = rc_bittree(rc,
                            lzma->dist_slot[lzma_get_dist_state(len)] - 1,
                            DIST_SLOTS) - DIST_SLOTS;

            if (dist_slot < DIST_MODEL_START) {
                dist = dist_slot;
            } else {
                uint32_t limit = (dist_slot >> 1) - 1;
                dist = (2 | (dist_slot & 1)) << limit;

                if (dist_slot < DIST_MODEL_END) {
                    dist += rc_bittree_reverse(rc,
                                lzma->dist_special + dist - dist_slot - 1,
                                limit);
                } else {
                    dist += rc_direct(rc, limit - ALIGN_BITS) << ALIGN_BITS;
                    dist += rc_bittree_reverse(rc, lzma->dist_align,
                                               ALIGN_BITS);
                }
            }

            lzma->rep0 = dist;
            lzma->state = lzma_state_match(lzma->state);

            if (dist >= dict->full || dist >= dict->size) {
                s->compressed -= (uint32_t)(rc->in_pos - b->in_pos);
                b->in_pos = rc->in_pos;
                return 0;
            }

            if (!dict_repeat(dict, &len, dist)) {
                s->match_len = len;
                break;
            }
        }
    }

    s->compressed -= (uint32_t)(rc->in_pos - b->in_pos);
    b->in_pos = rc->in_pos;
    return 1;
}

/*
 * Allocate an LZMA2 decoder.
 */
struct xz_dec_lzma2 *xz_dec_lzma2_create(enum xz_mode mode, uint32_t dict_max)
{
    struct xz_dec_lzma2 *s;

    s = (struct xz_dec_lzma2 *)malloc(sizeof(*s));
    if (s == NULL)
        return NULL;

    memset(s, 0, sizeof(*s));
    s->dict.mode = mode;
    s->dict.size = dict_max;

    if (mode == XZ_PREALLOC && dict_max > 0) {
        s->dict.buf = (uint8_t *)malloc(dict_max);
        if (s->dict.buf == NULL) {
            free(s);
            return NULL;
        }
        s->dict.allocated = dict_max;
    }

    return s;
}

/*
 * Reset the LZMA2 decoder for a new block.
 */
enum xz_ret xz_dec_lzma2_reset(struct xz_dec_lzma2 *s, uint8_t props)
{
    uint32_t dict_size;

    if (props > 40)
        return XZ_OPTIONS_ERROR;

    if (props == 40) {
        dict_size = 0xFFFFFFFF;
    } else {
        dict_size = 2 + (props & 1);
        dict_size <<= props / 2 + 11;
    }

    if (s->dict.mode != XZ_SINGLE && s->dict.size > 0 && dict_size > s->dict.size)
        return XZ_MEMLIMIT_ERROR;

    if (s->dict.mode == XZ_DYNALLOC) {
        if (s->dict.allocated < dict_size) {
            free(s->dict.buf);
            s->dict.buf = (uint8_t *)malloc(dict_size);
            if (s->dict.buf == NULL) {
                s->dict.allocated = 0;
                return XZ_MEM_ERROR;
            }
            s->dict.allocated = dict_size;
        }
    }

    if (s->dict.mode != XZ_SINGLE)
        s->dict.end = dict_size;

    s->sequence = SEQ_LZMA2_CONTROL;
    s->need_dict_reset = 1;
    s->need_lzma_init = 1;
    s->need_props = 1;
    s->match_len = 0;

    return XZ_OK;
}

/*
 * LZMA2 main decoding loop.
 */
enum xz_ret xz_dec_lzma2_run(struct xz_dec_lzma2 *s, struct xz_buf *b)
{
    for (;;) {
        switch (s->sequence) {

        case SEQ_LZMA2_CONTROL:
            if (b->in_pos >= b->in_size)
                return XZ_OK;

            s->control = b->in[b->in_pos++];

            if (s->control == 0x00)
                return XZ_STREAM_END;

            if (s->control < 0x03) {
                /*
                 * Uncompressed chunk:
                 * 0x01 = no dictionary reset
                 * 0x02 = dictionary reset
                 */
                if (s->control == 0x02) {
                    s->need_dict_reset = 0;
                    s->need_lzma_init = 1;
                    dict_reset(&s->dict, b);
                } else if (s->need_dict_reset) {
                    return XZ_DATA_ERROR;
                }
                s->sequence = SEQ_LZMA2_UNCOMPRESSED_1;
                break;
            }

            /*
             * LZMA chunk. The control byte encodes:
             *   0x80-0x9F: no reset
             *   0xA0-0xBF: state reset
             *   0xC0-0xDF: state reset + new properties
             *   0xE0-0xFF: full reset (state, props, dict)
             *
             * Bits 4..0 = uncompressed size high bits.
             */
            if (s->control >= 0xE0) {
                s->need_dict_reset = 0;
                dict_reset(&s->dict, b);
            } else if (s->need_dict_reset) {
                return XZ_DATA_ERROR;
            }

            if (s->control >= 0xA0)
                s->need_lzma_init = 1;

            if (s->control >= 0xC0) {
                s->need_props = 1;
            }

            s->uncompressed = (s->control & 0x1F) << 16;
            s->sequence = SEQ_LZMA2_UNCOMPRESSED_1;
            break;

        case SEQ_LZMA2_UNCOMPRESSED_1:
            if (b->in_pos >= b->in_size)
                return XZ_OK;

            s->uncompressed += (uint32_t)b->in[b->in_pos++] << 8;
            s->sequence = SEQ_LZMA2_UNCOMPRESSED_2;
            break;

        case SEQ_LZMA2_UNCOMPRESSED_2:
            if (b->in_pos >= b->in_size)
                return XZ_OK;

            s->uncompressed += (uint32_t)b->in[b->in_pos++] + 1;

            if (s->control < 0x03) {
                /* Uncompressed copy: "compressed" size == uncompressed size */
                s->compressed = s->uncompressed;
                s->sequence = SEQ_LZMA2_COPY;
                break;
            }

            /* LZMA chunk: read compressed size */
            s->sequence = SEQ_LZMA2_COMPRESSED_0;
            break;

        case SEQ_LZMA2_COMPRESSED_0:
            if (b->in_pos >= b->in_size)
                return XZ_OK;

            s->compressed = (uint32_t)b->in[b->in_pos++] << 8;
            s->sequence = SEQ_LZMA2_COMPRESSED_1;
            break;

        case SEQ_LZMA2_COMPRESSED_1:
            if (b->in_pos >= b->in_size)
                return XZ_OK;

            s->compressed += (uint32_t)b->in[b->in_pos++] + 1;

            if (s->need_props)
                s->sequence = SEQ_LZMA2_PROPS;
            else
                s->sequence = SEQ_LZMA2_LZMA_PREPARE;
            break;

        case SEQ_LZMA2_PROPS:
            if (b->in_pos >= b->in_size)
                return XZ_OK;
            {
                uint8_t props_byte = b->in[b->in_pos++];
                uint32_t lc, lp, pb;

                --s->compressed;

                if (props_byte >= 9 * 5 * 5)
                    return XZ_DATA_ERROR;

                lc = props_byte % 9;
                props_byte /= 9;
                lp = props_byte % 5;
                pb = props_byte / 5;

                if (lc + lp > 4)
                    return XZ_DATA_ERROR;

                s->lzma.lc = lc;
                s->lzma.literal_pos_mask = (1U << lp) - 1;
                s->lzma.pos_mask = (1U << pb) - 1;
                s->need_props = 0;
            }
            s->sequence = SEQ_LZMA2_LZMA_PREPARE;
            break;

        case SEQ_LZMA2_LZMA_PREPARE:
            if (s->need_lzma_init) {
                lzma_reset(&s->lzma);
                rc_reset(&s->lzma.rc);
                s->need_lzma_init = 0;
            }

            if (!rc_read_init(&s->lzma.rc, b))
                return XZ_OK;

            s->compressed -= 5;
            s->match_len = 0;
            s->sequence = SEQ_LZMA2_LZMA_RUN;
            break;

        case SEQ_LZMA2_LZMA_RUN: {
            size_t in_avail;

            dict_limit(&s->dict, s->uncompressed);

            in_avail = min_t(size_t, b->in_size - b->in_pos, s->compressed);

            if (!lzma_decode_loop(s, b, in_avail))
                return XZ_DATA_ERROR;

            {
                uint32_t produced = dict_flush(&s->dict, b);
                s->uncompressed -= produced;
            }

            if (s->uncompressed == 0) {
                /* Chunk complete. Compressed may still have trailing
                 * bytes from the range coder (up to 5). Skip them. */
                s->sequence = SEQ_LZMA2_CONTROL;
                break;
            }

            if (b->out_pos == b->out_size
                || (b->in_pos == b->in_size && s->compressed > 0))
                return XZ_OK;

            break;
        }

        case SEQ_LZMA2_COPY:
            dict_limit(&s->dict, s->uncompressed);
            dict_uncompressed(&s->dict, b, &s->compressed);
            {
                uint32_t produced = dict_flush(&s->dict, b);
                s->uncompressed -= produced;
            }

            if (s->uncompressed == 0) {
                s->sequence = SEQ_LZMA2_CONTROL;
                break;
            }

            if (b->in_pos == b->in_size || b->out_pos == b->out_size)
                return XZ_OK;

            break;

        default:
            return XZ_DATA_ERROR;
        }
    }
}

void xz_dec_lzma2_end(struct xz_dec_lzma2 *s)
{
    if (s != NULL) {
        if (s->dict.mode != XZ_SINGLE)
            free(s->dict.buf);
        free(s);
    }
}
