/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of src/pa_bias.c (#173): the X6200's PA bias from its base's calibration block. The block as ea4iln
 *  read it on a cold start (3326/3843 at bytes 281-284, low byte first) gives his radio's own, the same words as the
 *  register 41 he saw Xiegu's app send (0x0f030cfe) as the base keeps it; 3360/3820, what older versions sent, is
 *  taken and said to be either this radio's own or left over; out of range (0, 0xffff, the X6100's 450/650, one of
 *  the two only, a step past either end) both go back to 3360/3820; no block, or a short one, too. Then the edges of
 *  the range and the lines for the base log and Settings > Info.
 */

#include "pa_bias.h"

#include <stdio.h>
#include <string.h>

static int fails, checks;

static void check(bool ok, const char *what, long v) {
    checks++;
    if (!ok) {
        fails++;
        printf("FAIL %s: %ld\n", what, v);
    }
}

/* A block of filler with drive and final at their place, low byte first */
static void block_with(uint8_t *block, uint16_t drive, uint16_t final) {
    memset(block, 0x5a, PA_BIAS_BLOCK_LEN);
    block[280] = 0;     /* "open TX", 0 on a cold start */
    block[PA_BIAS_DRIVE_AT] = drive & 0xff;
    block[PA_BIAS_DRIVE_AT + 1] = drive >> 8;
    block[PA_BIAS_FINAL_AT] = final & 0xff;
    block[PA_BIAS_FINAL_AT + 1] = final >> 8;
}

static void expect(const char *what, uint16_t drive, uint16_t final, pa_bias_from_t from, uint16_t want_drive,
                   uint16_t want_final) {
    uint8_t   block[PA_BIAS_BLOCK_LEN];
    pa_bias_t b;
    char      name[96];

    block_with(block, drive, final);
    b = pa_bias_pick(block, sizeof(block));
    snprintf(name, sizeof(name), "%s: from", what);
    check(b.from == from, name, b.from);
    snprintf(name, sizeof(name), "%s: drive", what);
    check(b.drive == want_drive, name, b.drive);
    snprintf(name, sizeof(name), "%s: final", what);
    check(b.final == want_final, name, b.final);
    snprintf(name, sizeof(name), "%s: block's drive", what);
    check(b.block_drive == drive, name, b.block_drive);
    snprintf(name, sizeof(name), "%s: block's final", what);
    check(b.block_final == final, name, b.block_final);
}

int main(void) {
    uint8_t   block[PA_BIAS_BLOCK_LEN];
    pa_bias_t b;
    char      line[200];

    /* ea4iln's radio on a cold start, and the same as register 41's four bytes as the ARM sends them (0x0f030cfe,
     * low byte first) where the base keeps them: drive in the low half, final in the high one */
    expect("cold start", 3326, 3843, PA_BIAS_OWN, 3326, 3843);
    uint32_t reg41 = 0x0f030cfe;
    memset(block, 0, sizeof(block));
    for (int i = 0; i < 4; i++) {
        block[PA_BIAS_DRIVE_AT + i] = (uint8_t)(reg41 >> (8 * i));
    }
    b = pa_bias_pick(block, sizeof(block));
    check(b.from == PA_BIAS_OWN && b.drive == 3326 && b.final == 3843, "register 41 as kept", b.drive);

    /* What older versions (and gdyuldin's GUI) sent, left in the block: taken, said either way */
    expect("old fixed", 3360, 3820, PA_BIAS_OWN_OR_OLD, 3360, 3820);
    /* One of the two the same is a radio's own */
    expect("drive as fixed", 3360, 3843, PA_BIAS_OWN, 3360, 3843);
    expect("final as fixed", 3326, 3820, PA_BIAS_OWN, 3326, 3820);

    /* Out of range: both fixed */
    expect("zero", 0, 0, PA_BIAS_FIXED_RANGE, 3360, 3820);
    expect("erased", 0xffff, 0xffff, PA_BIAS_FIXED_RANGE, 3360, 3820);
    expect("X6100's 450/650 (before 0.3.4)", 450, 650, PA_BIAS_FIXED_RANGE, 3360, 3820);
    expect("drive zero", 0, 3843, PA_BIAS_FIXED_RANGE, 3360, 3820);
    expect("final erased", 3326, 0xffff, PA_BIAS_FIXED_RANGE, 3360, 3820);
    /* A base that kept them high byte first is refused, not misread */
    expect("stored high byte first", 0xfe0c, 0x030f, PA_BIAS_FIXED_RANGE, 3360, 3820);

    /* The edges: 10 % of the fixed ones, under 4096 */
    check(pa_bias_min(3360) == 3024 && pa_bias_max(3360) == 3696, "drive range", pa_bias_max(3360));
    check(pa_bias_min(3820) == 3438 && pa_bias_max(3820) == 4095, "final range", pa_bias_max(3820));
    expect("drive lowest", 3024, 3843, PA_BIAS_OWN, 3024, 3843);
    expect("drive below", 3023, 3843, PA_BIAS_FIXED_RANGE, 3360, 3820);
    expect("drive highest", 3696, 3843, PA_BIAS_OWN, 3696, 3843);
    expect("drive above", 3697, 3843, PA_BIAS_FIXED_RANGE, 3360, 3820);
    expect("final lowest", 3326, 3438, PA_BIAS_OWN, 3326, 3438);
    expect("final below", 3326, 3437, PA_BIAS_FIXED_RANGE, 3360, 3820);
    expect("final highest", 3326, 4095, PA_BIAS_OWN, 3326, 4095);
    expect("final 4096", 3326, 4096, PA_BIAS_FIXED_RANGE, 3360, 3820);

    /* No block, or one cut short */
    b = pa_bias_pick(NULL, 0);
    check(b.from == PA_BIAS_FIXED_UNREAD && b.drive == 3360 && b.final == 3820, "not read", b.from);
    block_with(block, 3326, 3843);
    b = pa_bias_pick(block, PA_BIAS_BLOCK_LEN - 1);
    check(b.from == PA_BIAS_FIXED_UNREAD && b.drive == 3360 && b.final == 3820, "short", b.from);

    /* The lines */
    block_with(block, 3326, 3843);
    b = pa_bias_pick(block, sizeof(block));
    pa_bias_log_line(&b, line, sizeof(line));
    printf("log:  %s\n", line);
    check(strstr(line, "3326/3843") && strstr(line, "own"), "log own", 0);
    pa_bias_info(&b, line, sizeof(line));
    printf("info: %s\n", line);
    check(!strcmp(line, "3326 / 3843, the radio's own"), "info own", 0);

    block_with(block, 3360, 3820);
    b = pa_bias_pick(block, sizeof(block));
    pa_bias_log_line(&b, line, sizeof(line));
    printf("log:  %s\n", line);
    check(!strcmp(line, "bias 3360/3820: either this radio's own or left by an older version until the base loses "
                        "power"), "log own or old", 0);
    pa_bias_info(&b, line, sizeof(line));
    printf("info: %s\n", line);

    block_with(block, 450, 650);
    b = pa_bias_pick(block, sizeof(block));
    pa_bias_log_line(&b, line, sizeof(line));
    printf("log:  %s\n", line);
    check(strstr(line, "450/650") && strstr(line, "drive 3024-3696, final 3438-4095") && strstr(line, "fixed 3360/3820"),
          "log range", 0);
    pa_bias_info(&b, line, sizeof(line));
    printf("info: %s\n", line);
    check(!strcmp(line, "3360 / 3820, fixed (base: 450 / 650)"), "info range", 0);
    /* The longest the Info page gets */
    block_with(block, 65535, 65535);
    b = pa_bias_pick(block, sizeof(block));
    pa_bias_info(&b, line, sizeof(line));
    printf("info: %s\n", line);
    check(strlen(line) < 48, "info fits radio_pa_bias_info's buffer", (long)strlen(line));

    b = pa_bias_pick(NULL, 0);
    pa_bias_log_line(&b, line, sizeof(line));
    printf("log:  %s\n", line);
    check(strstr(line, "could not be read") && strstr(line, "fixed 3360/3820"), "log unread", 0);
    pa_bias_info(&b, line, sizeof(line));
    printf("info: %s\n", line);

    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
