/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of src/cfg/lo_offset.c, where the waterfall and the spectrum place the base's frames, Hz from the dial
 *  (#191):
 *  - receive: on every base, in every mode, at every key tone the same as up to v0.3.13 (written out here as it was),
 *    whatever the zoom: a key tone below the dial in CW and above it in CW-R on the X6100, 0 in the other modes and on
 *    the X6200
 *  - TX on Xiegu's 1.1.8, a patched base and the X6200: the receive offset at every zoom, as up to v0.3.13
 *  - TX on Xiegu's 1.1.9: the receive offset over the zoom, at zoom 1 that of 1.1.8
 *  - TX on 1.1.9 against the reporter's screen (21 Nov, zoom 8, an IF shift of -3000 Hz): where the carrier was drawn
 *    with the TX frames placed on the dial (v0.3.13), from 500 to 1200 Hz in CW and CW-R, and on 6 Nov with them
 *    placed as in receive (v0.3.8), moved by the new offset: on the line to under a pixel; with v0.3.13's 3 pixels and
 *    more off, so the cases tell the difference
 *  See the Makefile.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "cfg/lo_offset.h"
#include "radio_types.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int fails, checks;

static void check(int ok, const char *what, long a, long b) {
    checks++;
    if (!ok) {
        fails++;
        printf("FAIL %s: %ld, %ld\n", what, a, b);
    }
}

/* The bases, by the HW_CAP_* bits hw_caps() gives them */
static const struct {
    const char *name;
    uint32_t    caps;
} bases[] = {
    {"Xiegu's 1.1.8", HW_CAP_TX_IQ_OFFSET},
    {"Xiegu's 1.1.9", HW_CAP_TX_IQ_OFFSET | HW_CAP_ZOOM | HW_CAP_IF_SHIFT | HW_CAP_TX_SPECTRUM_ON_DIAL},
    {"a patched base, rev 13", HW_CAP_TX_IQ_OFFSET | HW_CAP_ZOOM | HW_CAP_IF_SHIFT | HW_CAP_DNF_AUTO | HW_CAP_COMP |
                                   HW_CAP_COMP_SHAPE | HW_CAP_CODEC_GAIN | HW_CAP_TX_LEVEL | HW_CAP_BAND_GAIN |
                                   HW_CAP_FM_EMP | HW_CAP_TX_FILTER | HW_CAP_CESSB},
    {"the X6200", HW_CAP_PSD | HW_CAP_CW_SPECTRUM_ON_DIAL | HW_CAP_CW_TONE_RISES | HW_CAP_SAM_WFM | HW_CAP_KEEPALIVE},
};

#define BASES ((int)(sizeof(bases) / sizeof(bases[0])))

/* The receive offset up to v0.3.13 (settings_manager.cpp's lo_offset_compute() then), and the TX one before #191 */
static int32_t rx_before(int32_t mode, int32_t key_tone, uint32_t caps) {
    if (caps & HW_CAP_CW_SPECTRUM_ON_DIAL) return 0;
    if (mode == radio_mode_cw) return -key_tone;
    if (mode == radio_mode_cwr) return key_tone;
    return 0;
}

/* The zoom 8: 12.5 kHz over the 800 pixels */
#define ZOOM    8
#define PX_HZ   (100000.0 / ZOOM / 800)

/* The reporter's screen, 21 Nov (v0.3.13, 1.1.9, zoom 8, IF shift -3000 Hz): the peak of the spectrum's TX trace,
 * pixels right of the red line, at each key tone, read from the frames where it had settled. Less 0.5: a trace there
 * sits half a pixel right of the line, the receiver's own noise in receive drawn at 115.7 pixels for 1800 Hz above
 * the dial (115.2). The waterfall's newest rows the same to a pixel or two (the line covers 7 of them) */
static const struct {
    int32_t mode, tone;
    double  px;
} seen[] = {
    {radio_mode_cw, 1200, 10.1},  {radio_mode_cw, 1170, 10.1},  {radio_mode_cw, 1050, 9.4},
    {radio_mode_cw, 960, 8.5},    {radio_mode_cw, 910, 8.2},    {radio_mode_cw, 780, 7.0},
    {radio_mode_cw, 690, 6.2},    {radio_mode_cw, 590, 5.7},    {radio_mode_cw, 500, 4.6},
    {radio_mode_cwr, 500, -3.4},  {radio_mode_cwr, 600, -4.1},  {radio_mode_cwr, 670, -4.7},
    {radio_mode_cwr, 780, -5.3},  {radio_mode_cwr, 880, -6.1},  {radio_mode_cwr, 990, -7.2},
    {radio_mode_cwr, 1130, -8.1}, {radio_mode_cwr, 1200, -9.1},
};

#define SEEN_N ((int)(sizeof(seen) / sizeof(seen[0])))

int main(void) {
    printf("Receive, every base, mode, key tone and zoom: as up to v0.3.13\n");
    for (int b = 0; b < BASES; b++) {
        for (int32_t mode = 0; mode <= radio_mode_last; mode++) {
            for (int32_t tone = 300; tone <= 1200; tone += 10) {
                check(lo_offset_rx(mode, tone, bases[b].caps) == rx_before(mode, tone, bases[b].caps), bases[b].name,
                      mode, tone);
            }
        }
    }
    check(lo_offset_rx(radio_mode_cw, 700, bases[0].caps) == -700, "1.1.8, CW: the key tone below the dial", 0, 0);
    check(lo_offset_rx(radio_mode_cwr, 700, bases[1].caps) == 700, "1.1.9, CW-R: the key tone above the dial", 0, 0);
    check(lo_offset_rx(radio_mode_usb, 700, bases[1].caps) == 0, "1.1.9, USB: on the dial", 0, 0);
    check(lo_offset_rx(radio_mode_cw, 700, bases[3].caps) == 0, "the X6200, CW: on the dial", 0, 0);

    printf("TX on 1.1.8, a patched base and the X6200: the receive offset at every zoom, as up to v0.3.13\n");
    for (int b = 0; b < BASES; b++) {
        if (bases[b].caps & HW_CAP_TX_SPECTRUM_ON_DIAL) continue;
        for (int32_t mode = 0; mode <= radio_mode_last; mode++) {
            for (int32_t tone = 300; tone <= 1200; tone += 10) {
                for (int32_t zoom = 1; zoom <= 8; zoom++) {
                    check(lo_offset_tx(mode, tone, bases[b].caps, zoom) == rx_before(mode, tone, bases[b].caps),
                          bases[b].name, mode, tone * 100 + zoom);
                }
            }
        }
    }

    printf("TX on 1.1.9: the receive offset over the zoom, at zoom 1 1.1.8's\n");
    uint32_t v119 = bases[1].caps;

    for (int32_t mode = 0; mode <= radio_mode_last; mode++) {
        for (int32_t tone = 300; tone <= 1200; tone += 10) {
            int32_t rx = lo_offset_rx(mode, tone, v119);

            for (int32_t zoom = 1; zoom <= 8; zoom++) {
                int32_t tx = lo_offset_tx(mode, tone, v119, zoom);

                check(abs(tx * zoom - rx) < zoom, "1.1.9: the receive offset over the zoom", tx, zoom);
            }
            check(lo_offset_tx(mode, tone, v119, 1) == lo_offset_tx(mode, tone, bases[0].caps, 1),
                  "1.1.9 at zoom 1: as 1.1.8", mode, tone);
            check(lo_offset_tx(mode, tone, v119, 0) == rx, "a zoom of 0 taken as 1", mode, tone);
        }
    }
    check(lo_offset_tx(radio_mode_cw, 1200, v119, 8) == -150, "1.1.9, CW, 1200 Hz, zoom 8", 0, 0);
    check(lo_offset_tx(radio_mode_cwr, 1200, v119, 8) == 150, "1.1.9, CW-R, 1200 Hz, zoom 8", 0, 0);
    check(lo_offset_tx(radio_mode_usb, 1200, v119, 8) == 0, "1.1.9, USB: on the dial", 0, 0);

    printf("TX on 1.1.9, the reporter's screen: the carrier on the line with the new offset, off it before\n");
    for (int i = 0; i < SEEN_N; i++) {
        /* Drawn with the frame placed on the dial: where the carrier is from the frame's middle as drawn */
        double carrier = (seen[i].px - 0.5) * PX_HZ;
        double now     = (carrier + lo_offset_tx(seen[i].mode, seen[i].tone, v119, ZOOM)) / PX_HZ;
        double before  = carrier / PX_HZ;

        printf("  %s %4d Hz: %+5.1f px before, %+5.2f px now\n", seen[i].mode == radio_mode_cw ? "CW  " : "CW-R",
               seen[i].tone, before, now);
        check(fabs(now) < 0.75, "the carrier on the line", seen[i].tone, lround(now * 100));
        check(fabs(before) > 3.0, "v0.3.13's off it", seen[i].tone, lround(before * 100));
    }

    /* 6 Nov (v0.3.8, 1.1.9, zoom 8): the frames placed as in receive, a key tone of 580 Hz below the dial; the
     * carrier about 32 pixels below the line in CW, read to a pixel */
    double carrier = -32 * PX_HZ - rx_before(radio_mode_cw, 580, v119);
    double now     = (carrier + lo_offset_tx(radio_mode_cw, 580, v119, ZOOM)) / PX_HZ;

    printf("  CW    580 Hz, v0.3.8: -32 px before, %+5.2f px now\n", now);
    check(fabs(now) <= 1.0, "v0.3.8's carrier on the line", 580, lround(now * 100));

    printf("lo_offset: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
