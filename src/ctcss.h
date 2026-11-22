/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - a CTCSS tone under the audio the radio's Linux side transmits in FM (#141): the Simplex repeater of
 *  the internet voice apps (voice_net.c). Neither base has a tone of its own, and the hand mic's voice never passes
 *  Linux, so a tone goes out only with audio that does. Apart from the GUI so a PC can test it (tools/ctcss).
 *
 *  The level: the base's FM transmitter lifts the higher audio, so each tone has its own. Measured on an X6200 with
 *  a test set (AndyKeir, #141, 29.5 and 52.5 MHz): 91.5 Hz at -22.5 dBFS and 123.0 Hz at -24.0 dBFS both gave 350 to
 *  380 Hz of deviation. The line through those two gives the others; not measured on an X6100.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CTCSS_OFF 0

typedef struct {
    double phase; /* radians, kept from one block to the next: no click at a block's edge */
    double step;  /* radians a sample; 0: no tone */
    float  amp;   /* peak, in int16 units */
} ctcss_t;

/* The standard tones, in tenths of a Hz (67.0 Hz is 670), lowest first */
int ctcss_count(void);
int ctcss_tone(int i);

/* A setting's value made one of the tones: itself when it is one, else CTCSS_OFF */
int ctcss_valid(int tenths);

/* The next tone up (dir > 0) or down, CTCSS_OFF between the highest and the lowest */
int ctcss_step(int tenths, int dir);

/* The tone's peak level in dBFS for 350 to 380 Hz of deviation */
float ctcss_level_db(int tenths);

/* A tone (CTCSS_OFF, or not a tone: none) at a sample rate; the phase starts at 0 */
void ctcss_init(ctcss_t *c, int tenths, unsigned rate);

/* The tone added to n samples, clipped to int16; nothing with no tone */
void ctcss_mix(ctcss_t *c, int16_t *buf, size_t n);

#ifdef __cplusplus
}
#endif
