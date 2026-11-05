/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the X6200's PA bias from its base's calibration block (pa_bias.c, #173). Each base has its own
 *  factory values there; Xiegu's app reads them at the start and sends them in register 41. tools/pa_bias checks
 *  the reading and the guard. The X6100 sends params.c's 450/650 as before
 *
 *  Copyright (c) 2026 RF.Guru
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef BOARD_X6200

#define PA_BIAS_BLOCK_LEN   507     /* the calibration block: 507 bytes from the base's I2C register 0x100 */
#define PA_BIAS_DRIVE_AT    281     /* the driver's bias in it, 16 bits little-endian, */
#define PA_BIAS_FINAL_AT    283     /* then the final's: register 41's two halves (drive low, final high) */

/* What versions 0.3.4 to 0.3.8 always sent, from a capture of another X6200 (0x0eec0d20); the fallback */
#define PA_BIAS_FIXED_DRIVE 3360
#define PA_BIAS_FIXED_FINAL 3820

/* A value from the block is taken within this many % of the fixed one and under PA_BIAS_TOP */
#define PA_BIAS_SPREAD_PCT  10
#define PA_BIAS_TOP         4096

typedef enum {
    PA_BIAS_OWN,            /* the block's */
    PA_BIAS_OWN_OR_OLD,     /* the block's, exactly the fixed ones: this radio's own or left by an older version */
    PA_BIAS_FIXED_RANGE,    /* the block's out of range: the fixed ones */
    PA_BIAS_FIXED_UNREAD,   /* no block: the fixed ones */
} pa_bias_from_t;

typedef struct {
    uint16_t        drive, final;               /* to send */
    uint16_t        block_drive, block_final;   /* as the block has them; 0 when not read */
    pa_bias_from_t  from;
} pa_bias_t;

/* The bias to send, from the block (NULL or shorter than PA_BIAS_BLOCK_LEN: not read) */
pa_bias_t pa_bias_pick(const uint8_t *block, size_t len);

/* The lowest and highest value taken from the block for a fixed value */
uint16_t pa_bias_min(uint16_t fixed);
uint16_t pa_bias_max(uint16_t fixed);

/* The line for the base log, and the short one for Settings > Info */
void pa_bias_log_line(const pa_bias_t *b, char *buf, size_t size);
void pa_bias_info(const pa_bias_t *b, char *buf, size_t size);

#endif

#ifdef __cplusplus
}
#endif
