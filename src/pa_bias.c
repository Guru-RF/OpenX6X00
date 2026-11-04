/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the X6200's PA bias from its base's calibration block (pa_bias.h, #173). Xiegu's app reads the block
 *  (507 bytes from I2C register 0x100) and logs "Bias drive" and "Bias final" from 16-bit words at its bytes 281 and
 *  283, low byte first (seen in the disassembly of its 1.0.7); ea4iln read 3326/3843 there on a cold start of his
 *  radio and saw its 1.0.8 send register 41 = 0x0f030cfe, the same pair. The base also keeps in those bytes what was
 *  last written to register 41, until it loses power: after 3360/3820 went out (an older version, or the fallback
 *  here) the block shows those.
 *
 *  The guard: the two X6200s seen are 1 % apart (3360/3820 and 3326/3843). A value is taken within 10 % of the fixed
 *  one and under 4096 (all four fit 12 bits, though the field is 16 wide); 0 and 0xffff, a block never written or
 *  not read right, and the X6100's 450/650 that versions before 0.3.4 sent fall outside. Out of range, both go back
 *  to the fixed ones: the two belong together
 *
 *  Copyright (c) 2026 RF.Guru
 */

#include "pa_bias.h"

#ifdef BOARD_X6200

#include <stdio.h>

static uint16_t word_at(const uint8_t *block, size_t at) {
    return (uint16_t)(block[at] | block[at + 1] << 8);
}

uint16_t pa_bias_min(uint16_t fixed) {
    return (uint16_t)((uint32_t)fixed * (100 - PA_BIAS_SPREAD_PCT) / 100);
}

uint16_t pa_bias_max(uint16_t fixed) {
    uint32_t max = (uint32_t)fixed * (100 + PA_BIAS_SPREAD_PCT) / 100;

    return (uint16_t)(max < PA_BIAS_TOP ? max : PA_BIAS_TOP - 1);
}

static bool sane(uint16_t v, uint16_t fixed) {
    return v >= pa_bias_min(fixed) && v <= pa_bias_max(fixed);
}

pa_bias_t pa_bias_pick(const uint8_t *block, size_t len) {
    pa_bias_t b = { .drive = PA_BIAS_FIXED_DRIVE, .final = PA_BIAS_FIXED_FINAL, .from = PA_BIAS_FIXED_UNREAD };

    if (!block || len < PA_BIAS_BLOCK_LEN) {
        return b;
    }
    b.block_drive = word_at(block, PA_BIAS_DRIVE_AT);
    b.block_final = word_at(block, PA_BIAS_FINAL_AT);
    if (!sane(b.block_drive, PA_BIAS_FIXED_DRIVE) || !sane(b.block_final, PA_BIAS_FIXED_FINAL)) {
        b.from = PA_BIAS_FIXED_RANGE;
        return b;
    }
    b.drive = b.block_drive;
    b.final = b.block_final;
    b.from = b.drive == PA_BIAS_FIXED_DRIVE && b.final == PA_BIAS_FIXED_FINAL ? PA_BIAS_OWN_OR_OLD : PA_BIAS_OWN;
    return b;
}

void pa_bias_log_line(const pa_bias_t *b, char *buf, size_t size) {
    switch (b->from) {
        case PA_BIAS_OWN:
            snprintf(buf, size, "bias %u/%u (drive/final): this radio's own, from the base's calibration block",
                     b->drive, b->final);
            break;
        case PA_BIAS_OWN_OR_OLD:
            snprintf(buf, size, "bias %u/%u: either this radio's own or left by an older version until the base "
                     "loses power", b->drive, b->final);
            break;
        case PA_BIAS_FIXED_RANGE:
            snprintf(buf, size, "bias %u/%u in the base's calibration block is out of range (drive %u-%u, final "
                     "%u-%u): the fixed %u/%u instead", b->block_drive, b->block_final,
                     pa_bias_min(PA_BIAS_FIXED_DRIVE), pa_bias_max(PA_BIAS_FIXED_DRIVE),
                     pa_bias_min(PA_BIAS_FIXED_FINAL), pa_bias_max(PA_BIAS_FIXED_FINAL), b->drive, b->final);
            break;
        case PA_BIAS_FIXED_UNREAD:
            snprintf(buf, size, "bias: the base's calibration block could not be read: the fixed %u/%u instead",
                     b->drive, b->final);
            break;
    }
}

void pa_bias_info(const pa_bias_t *b, char *buf, size_t size) {
    switch (b->from) {
        case PA_BIAS_OWN:
            snprintf(buf, size, "%u / %u, the radio's own", b->drive, b->final);
            break;
        case PA_BIAS_OWN_OR_OLD:
            snprintf(buf, size, "%u / %u, own or an old version's", b->drive, b->final);
            break;
        case PA_BIAS_FIXED_RANGE:
            snprintf(buf, size, "%u / %u, fixed (base: %u / %u)", b->drive, b->final, b->block_drive,
                     b->block_final);
            break;
        case PA_BIAS_FIXED_UNREAD:
            snprintf(buf, size, "%u / %u, fixed (not read)", b->drive, b->final);
            break;
    }
}

#endif
