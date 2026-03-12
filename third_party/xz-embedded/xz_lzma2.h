/*
 * LZMA2 decoder - Internal constants and structures
 *
 * Based on xz-embedded by Lasse Collin (public domain).
 */

#ifndef XZ_LZMA2_H
#define XZ_LZMA2_H

#include "xz_private.h"

/* Range coder constants */
#define RC_SHIFT_BITS       8
#define RC_TOP_BITS         24
#define RC_TOP_VALUE        (1U << RC_TOP_BITS)
#define RC_BIT_MODEL_TOTAL_BITS 11
#define RC_BIT_MODEL_TOTAL  (1 << RC_BIT_MODEL_TOTAL_BITS)
#define RC_MOVE_BITS        5

/*
 * Maximum number of position bits (lp + pb combined).
 * LZMA uses up to 4 position bits.
 */
#define POS_STATES_MAX      (1 << 4)

/*
 * The LZMA state machine has 12 states. States 0-6 indicate that the
 * previous output was a literal; states 7-11 indicate a match/rep.
 */
#define STATES              12

/* Special state values */
#define LIT_STATES          7

/* Match length constants */
#define MATCH_LEN_MIN       2
#define LEN_LOW_BITS        3
#define LEN_LOW_SYMBOLS     (1 << LEN_LOW_BITS)
#define LEN_MID_BITS        3
#define LEN_MID_SYMBOLS     (1 << LEN_MID_BITS)
#define LEN_HIGH_BITS       8
#define LEN_HIGH_SYMBOLS    (1 << LEN_HIGH_BITS)

/* Total number of match length probabilities */
#define LEN_SYMBOLS         (LEN_LOW_SYMBOLS + LEN_MID_SYMBOLS + LEN_HIGH_SYMBOLS)

/* Distance slots */
#define DIST_STATES         4
#define DIST_SLOT_BITS      6
#define DIST_SLOTS          (1 << DIST_SLOT_BITS)
#define DIST_MODEL_START    4
#define DIST_MODEL_END      14
#define FULL_DISTANCES      (1 << (DIST_MODEL_END / 2))

/* Alignment bits for distance decoding */
#define ALIGN_BITS          4
#define ALIGN_SIZE          (1 << ALIGN_BITS)
#define ALIGN_MASK          (ALIGN_SIZE - 1)

/* Total number of LZMA probability variables */
#define PROBS_SIZE_LIT(lc)  (3U << (lc))

/* Literal coder */
#define LITERAL_CODERS_MAX  (1 << 4)   /* max lc = 4 */

/* LZMA2 control byte values */
#define LZMA2_CONTROL_LZMA          0x80
#define LZMA2_CONTROL_COPY_NO_RESET 0x01
#define LZMA2_CONTROL_COPY_RESET    0x02

/*
 * lzma_state_is_literal: Returns true if the given LZMA state indicates
 * that the previous symbol was a literal.
 */
static inline int lzma_state_is_literal(uint32_t state)
{
    return state < LIT_STATES;
}

/*
 * State transitions after different symbol types.
 */
static inline uint32_t lzma_state_literal(uint32_t state)
{
    if (state <= 3)
        return 0;
    if (state <= 9)
        return state - 3;
    return state - 6;
}

static inline uint32_t lzma_state_match(uint32_t state)
{
    return state < LIT_STATES ? 7 : 10;
}

static inline uint32_t lzma_state_long_rep(uint32_t state)
{
    return state < LIT_STATES ? 8 : 11;
}

static inline uint32_t lzma_state_short_rep(uint32_t state)
{
    return state < LIT_STATES ? 9 : 11;
}

/*
 * Get the distance state (0..3) from the match length.
 */
static inline uint32_t lzma_get_dist_state(uint32_t len)
{
    return len < DIST_STATES + MATCH_LEN_MIN
        ? len - MATCH_LEN_MIN : DIST_STATES - 1;
}

#endif /* XZ_LZMA2_H */
