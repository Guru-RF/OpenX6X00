/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of the FT8 app's Auto Sync decisions (src/ft8/time_sync.c). Decodes as ft8_window.c gives them: the
 *  decoder's audio stamped at its start (by CLOCK_REALTIME and CLOCK_MONOTONIC), the signal's time into it as the
 *  decoder reports it (its lag of 1.2 symbols on top), the sender's call; then the check once a slot, as on_tick
 *  makes it. Each case is a band: stations whose signals show a DT of their own on a right clock, a clock off by so
 *  much, the decoder's audio starting late after a long last decode. What must come out: the clock left alone when
 *  it is right, moved by its error when it is off (to a hundredth), never on too few stations or slots, nor when the
 *  stations disagree, nor further than 2 s; one vote a station (a Fox's many decodes count once), outliers outvoted;
 *  decodes of a clock set since dropped, also when they come in after the set; the window of 4 minutes; FT4 as FT8,
 *  FT2 not at all; the moment just past a slot's middle, the clock staying inside the slot.
 *
 *    sync_test
 */

#include "ft8/time_sync.h"
#include "ft8/ft2.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define NS      1000000000LL
#define T0      (1763200800LL * NS)     /* a minute's start (2025-11-15 10:00 UTC) */
#define OFS     (1000LL * NS)           /* CLOCK_REALTIME - CLOCK_MONOTONIC */
#define FT8     ((int)FTX_PROTOCOL_FT8)
#define FT4     ((int)FTX_PROTOCOL_FT4)
#define FT2     ((int)FTX_PROTOCOL_FT2)

static int bad, cases;

static void check(bool ok, const char *what) {
    cases++;
    if (!ok) {
        printf("WRONG %s\n", what);
        bad++;
    }
}

static bool near(float a, float b, float within) {
    return fabsf(a - b) <= within;
}

static int64_t slot_ns(int proto) {
    return llround(ftx_slot_time(proto) * 1e9);
}

static float symbol(int proto) {
    return proto == FT8 ? FT8_SYMBOL_PERIOD : FT4_SYMBOL_PERIOD;
}

/* A band as heard by this radio */
typedef struct {
    int     proto;
    float   clock;      /* our clock's error, s: + ahead */
    float   start;      /* the decoder's audio begins this long after the slot's start by our clock, s */
    int64_t ofs;        /* CLOCK_REALTIME - CLOCK_MONOTONIC */
} band_t;

static ftx_sync_t sync;

/* A decode in slot n (from T0) of call, whose signal shows dt on a right clock (FTX_SYNC_RX_DT_S on time); true if
 * it counted */
static bool hear(const band_t *b, int n, const char *call, float dt) {
    int64_t slot0    = T0 + n * slot_ns(b->proto);         /* the slot's start by our clock */
    int64_t start    = slot0 + llround(b->start * 1e9);    /* the decoder's audio, by our clock */
    float   at       = FTX_SYNC_START_S + dt + b->clock;   /* the signal, by our clock from the slot's start */
    float   time_sec = at - b->start + FTX_SYNC_DECODER_LAG * symbol(b->proto);
    int64_t now      = slot0 + slot_ns(b->proto);          /* the slot's last decode */

    return ftx_sync_add(&sync, b->proto, start, start - b->ofs, time_sec, call, now, now - b->ofs);
}

/* The check in slot n, just past its middle */
static ftx_sync_rc_t look(const band_t *b, int n, ftx_sync_result_t *r) {
    int64_t now = T0 + n * slot_ns(b->proto) + slot_ns(b->proto) / 2 + 50000000LL;

    return ftx_sync_check(&sync, b->proto, now, now - b->ofs, r);
}

static const char *calls[] = {"ON4ABC", "PA3XYZ", "DL1AAA", "G4BBB", "F5CCC", "EA2DDD", "I1EEE", "OH2FFF", "SM5GGG",
                              "OK1HHH", "SP3III", "HB9JJJ"};

/* Slots 0 .. slots-1, stations 0 .. stations-1 each slot, with the DTs dts[] in turn */
static void band(const band_t *b, int slots, int stations, const float *dts, int n_dts) {
    for (int n = 0; n < slots; n++)
        for (int k = 0; k < stations; k++) hear(b, n, calls[k], dts[(n * stations + k) % n_dts]);
}

static const float right[] = {0.10f, 0.22f, 0.15f, 0.08f, 0.19f, 0.14f, 0.25f, 0.12f, 0.17f};

static void right_and_off(int proto) {
    char              what[96];
    ftx_sync_result_t r;
    const char       *name = ftx_protocol_name(proto);

    /* A right clock: left alone */
    band_t b = {.proto = proto, .clock = 0.0f, .start = 0.2f, .ofs = OFS};

    ftx_sync_reset(&sync);
    band(&b, 4, 7, right, 9);
    snprintf(what, sizeof(what), "%s right clock: on time", name);
    check(look(&b, 4, &r) == FTX_SYNC_ON_TIME, what);
    snprintf(what, sizeof(what), "%s right clock: 7 stations, 4 slots, 28 decodes", name);
    check(r.stations == 7 && r.slots == 4 && r.decodes == 28, what);
    snprintf(what, sizeof(what), "%s right clock: the median DT is the band's (0.15 s)", name);
    check(near(r.median_dt, 0.15f, 0.02f), what);

    /* Off by these: moved by its error, to a hundredth; the decoder's audio starting at another place each slot
     * (after a quick last decode, after a long one) changes nothing */
    static const float offs[] = {0.6f, -0.8f, 1.9f, -1.4f, 0.25f, -0.21f};

    for (size_t i = 0; i < sizeof(offs) / sizeof(offs[0]); i++) {
        ftx_sync_reset(&sync);
        for (int n = 0; n < 4; n++) {
            band_t bn = {.proto = proto, .clock = offs[i], .start = n % 2 ? 0.75f : 0.1f, .ofs = OFS};

            for (int k = 0; k < 6; k++) hear(&bn, n, calls[k], right[(n * 6 + k) % 9]);
        }
        b.clock = offs[i];
        snprintf(what, sizeof(what), "%s clock %+.2f s: a step", name, offs[i]);
        check(look(&b, 4, &r) == FTX_SYNC_STEP, what);
        snprintf(what, sizeof(what), "%s clock %+.2f s: moved by %+.2f s (%+.3f)", name, offs[i], -offs[i], r.step);
        check(near(r.step, -offs[i], 0.02f), what);
    }

    /* 0.15 s off: under 0.2 s, left alone (0.25 s and 0.21 s above were steps) */
    b.clock = 0.15f;
    ftx_sync_reset(&sync);
    band(&b, 4, 6, right, 9);
    snprintf(what, sizeof(what), "%s clock 0.15 s ahead: on time (under 0.2 s)", name);
    check(look(&b, 4, &r) == FTX_SYNC_ON_TIME, what);

    /* 2.5 s off: too far, no step */
    b.clock = 2.5f;
    ftx_sync_reset(&sync);
    band(&b, 4, 6, right, 9);
    snprintf(what, sizeof(what), "%s clock 2.5 s ahead: too far", name);
    check(look(&b, 4, &r) == FTX_SYNC_TOO_FAR && near(r.step, -2.5f, 0.02f), what);
}

static void enough(void) {
    ftx_sync_result_t r;
    band_t            b = {.proto = FT8, .clock = 0.6f, .start = 0.2f, .ofs = OFS};

    /* 4 stations over 6 slots: not enough */
    ftx_sync_reset(&sync);
    band(&b, 6, 4, right, 9);
    check(look(&b, 6, &r) == FTX_SYNC_WAIT && r.stations == 4, "4 stations: wait");

    /* A fifth, heard in one slot: now enough */
    hear(&b, 5, calls[4], 0.15f);
    check(look(&b, 6, &r) == FTX_SYNC_STEP && r.stations == 5, "a fifth station: a step");

    /* 8 stations in 2 slots: wait; in the third a decision */
    ftx_sync_reset(&sync);
    band(&b, 2, 8, right, 9);
    check(look(&b, 2, &r) == FTX_SYNC_WAIT && r.slots == 2 && r.stations == 8, "2 slots: wait");
    for (int k = 0; k < 8; k++) hear(&b, 2, calls[k], right[k]);
    check(look(&b, 3, &r) == FTX_SYNC_STEP && r.slots == 3, "3 slots: a step");
}

static void votes(void) {
    ftx_sync_result_t r;
    band_t            b = {.proto = FT8, .clock = 0.0f, .start = 0.2f, .ofs = OFS};

    /* A Fox: its 5 streams a slot, its own clock 1.5 s off, and 4 Hounds on time. One vote: the clock is right */
    ftx_sync_reset(&sync);
    for (int n = 0; n < 4; n++) {
        for (int k = 0; k < 5; k++) hear(&b, n, "VP6FOX", 1.65f);
        for (int k = 0; k < 4; k++) hear(&b, n, calls[k], right[k]);
    }
    check(look(&b, 4, &r) == FTX_SYNC_ON_TIME && r.stations == 5 && r.decodes == 36,
          "a Fox's 20 decodes count once: on time");

    /* 7 stations, 2 of them with clocks far off (+1.2 s, -0.9 s): outvoted */
    static const float some_off[] = {0.12f, 0.18f, 1.35f, 0.15f, -0.75f, 0.10f, 0.20f};

    b.clock = 0.5f;
    ftx_sync_reset(&sync);
    band(&b, 4, 7, some_off, 7);
    check(look(&b, 4, &r) == FTX_SYNC_STEP && near(r.step, -0.5f, 0.03f), "two clocks far off: outvoted");

    /* Two camps: 3 stations at -0.3 s, 3 at +0.9 s. No step */
    static const float camps[] = {-0.3f, -0.3f, -0.3f, 0.9f, 0.9f, 0.9f};

    b.clock = 0.0f;
    ftx_sync_reset(&sync);
    band(&b, 4, 6, camps, 6);
    check(look(&b, 4, &r) == FTX_SYNC_SPREAD && r.spread > FTX_SYNC_SPREAD_MAX_S, "two camps: they disagree");
}

static void clock_set(void) {
    ftx_sync_result_t r;
    band_t            b = {.proto = FT8, .clock = 0.6f, .start = 0.2f, .ofs = OFS};

    /* The clock set (Time Sync, the web page, ntpd) after 4 slots: what was gathered is of the clock before */
    ftx_sync_reset(&sync);
    band(&b, 4, 6, right, 9);
    band_t after = b;

    after.ofs   = OFS - 600000000LL;    /* set back 0.6 s */
    after.clock = 0.0f;
    check(look(&after, 4, &r) == FTX_SYNC_WAIT && r.decodes == 0, "the clock set: what came before is dropped");

    /* A decode whose audio began before the set and comes in after it: not counted */
    int64_t start = T0 + 5 * slot_ns(FT8) + 200000000LL, now = start + slot_ns(FT8);

    check(!ftx_sync_add(&sync, FT8, start, start - OFS, 1.0f, "ON4ABC", now, now - after.ofs),
          "a decode of the clock before, coming after the set: not counted");

    /* From then on the new clock's: right, so on time */
    band(&after, 4, 6, right, 9);
    for (int n = 4; n < 8; n++)
        for (int k = 0; k < 6; k++) hear(&after, n, calls[k], right[k]);
    check(look(&after, 8, &r) == FTX_SYNC_ON_TIME && r.decodes == 48, "after the set: the new clock's decodes");

    /* A set of 5 ms is no set (two clock reads apart): counted */
    band_t jitter = b;

    jitter.ofs = OFS + 5000000LL;
    ftx_sync_reset(&sync);
    band(&b, 2, 6, right, 9);
    for (int n = 2; n < 4; n++)
        for (int k = 0; k < 6; k++) hear(&jitter, n, calls[k], right[k]);
    check(look(&jitter, 4, &r) == FTX_SYNC_STEP && r.decodes == 24, "5 ms between the clocks: no set");
}

static void window(void) {
    ftx_sync_result_t r;
    band_t            b = {.proto = FT8, .clock = 0.6f, .start = 0.2f, .ofs = OFS};

    /* 4 slots heard, then a quiet band: 4 minutes on they are gone */
    ftx_sync_reset(&sync);
    band(&b, 4, 6, right, 9);
    check(look(&b, 12, &r) == FTX_SYNC_STEP && r.decodes == 24, "after 3 minutes still counted");
    check(look(&b, 20, &r) == FTX_SYNC_WAIT && r.decodes == 0, "after 5 minutes gone");

    /* Full: the oldest go, the newest count */
    ftx_sync_reset(&sync);
    for (int n = 0; n < 100; n++)
        for (int k = 0; k < 8; k++) hear(&b, n, calls[k], right[k]);
    check(sync.n == FTX_SYNC_SAMPLES, "800 decodes: 512 kept");
    check(look(&b, 100, &r) == FTX_SYNC_STEP && r.decodes == 8 * 15, "of those, the last 4 minutes' count");
}

static void modes(void) {
    ftx_sync_result_t r;
    band_t            b8 = {.proto = FT8, .clock = 0.6f, .start = 0.2f, .ofs = OFS};
    band_t            b4 = {.proto = FT4, .clock = 0.6f, .start = 0.2f, .ofs = OFS};
    band_t            b2 = {.proto = FT2, .clock = 0.6f, .start = 0.2f, .ofs = OFS};

    /* FT8's decodes, then FT4: starts over */
    ftx_sync_reset(&sync);
    band(&b8, 4, 6, right, 9);
    check(look(&b4, 40, &r) == FTX_SYNC_WAIT && r.decodes == 0, "FT8 to FT4: starts over");

    /* FT2: not counted, never a step */
    ftx_sync_reset(&sync);
    check(!hear(&b2, 0, calls[0], 0.15f), "FT2: not counted");
    band(&b2, 8, 8, right, 9);
    check(look(&b2, 8, &r) == FTX_SYNC_WAIT && r.decodes == 0, "FT2: no step");
    check(!ftx_sync_moment(FT2, 1.9f, 0.3f), "FT2: no moment");

    /* No call, no stamp, a DT past 4 s: not counted */
    int64_t start = T0 + 200000000LL, now = start + slot_ns(FT8);

    ftx_sync_reset(&sync);
    check(!ftx_sync_add(&sync, FT8, start, start - OFS, 0.9f, "", now, now - OFS), "no call: not counted");
    check(!ftx_sync_add(&sync, FT8, start, start - OFS, 0.9f, NULL, now, now - OFS), "NULL call: not counted");
    check(!ftx_sync_add(&sync, FT8, 0, 0, 0.9f, "ON4ABC", now, now - OFS), "no stamp: not counted");
    check(!ftx_sync_add(&sync, FT8, start, start - OFS, 6.0f, "ON4ABC", now, now - OFS), "DT 5 s: not counted");
    check(!ftx_sync_add(&sync, FT8, start, start - OFS, NAN, "ON4ABC", now, now - OFS), "DT NaN: not counted");
}

/* The DT of a signal, whatever the decoder's audio began at: the slot's start by the clock */
static void dt(void) {
    /* An FT8 signal 0.3 s late (0.8 s into the slot), the audio begun 0.05, 0.6 and 1.1 s in, or 0.3 s before the
     * slot (a buffer started at the end of the slot before): DT 0.3 every time */
    static const float starts[] = {0.05f, 0.6f, 1.1f, -0.3f};

    for (size_t i = 0; i < sizeof(starts) / sizeof(starts[0]); i++) {
        int64_t start    = T0 + 4 * slot_ns(FT8) + llround(starts[i] * 1e9);
        float   time_sec = 0.8f - starts[i] + FTX_SYNC_DECODER_LAG * FT8_SYMBOL_PERIOD;
        char    what[64];

        snprintf(what, sizeof(what), "FT8 audio from %+.2f s: DT 0.30 (%.3f)", starts[i],
                 ftx_sync_dt(FT8, start, time_sec));
        check(near(ftx_sync_dt(FT8, start, time_sec), 0.3f, 0.001f), what);
    }
    /* FT4's slot of 7.5 s starts at :07.5 too */
    int64_t start = T0 + 7 * slot_ns(FT4) + 150000000LL;

    check(near(ftx_sync_dt(FT4, start, 0.65f - 0.15f + FTX_SYNC_DECODER_LAG * FT4_SYMBOL_PERIOD), 0.15f, 0.001f),
          "FT4 slot at :52.5: DT 0.15");
}

/* The moment: just past the middle, the clock staying 1 s inside the slot */
static void moment(void) {
    check(ftx_sync_moment(FT8, 7.55f, 2.0f) && ftx_sync_moment(FT8, 7.55f, -2.0f), "FT8 7.55 s, 2 s either way: now");
    check(!ftx_sync_moment(FT8, 7.4f, 0.5f), "FT8 7.4 s: before the middle");
    check(!ftx_sync_moment(FT8, 8.6f, 0.5f), "FT8 8.6 s: too long past the middle");
    check(!ftx_sync_moment(FT8, 13.3f, -0.5f), "FT8 13.3 s (after our transmission): not now");
    check(ftx_sync_moment(FT4, 3.8f, -2.0f) && ftx_sync_moment(FT4, 3.8f, 2.0f), "FT4 3.8 s, 2 s either way: now");
    check(!ftx_sync_moment(FT4, 3.8f, 3.0f), "FT4 3.8 s, 3 s on: past the slot's end");
    check(!ftx_sync_moment(FT4, 3.8f, -3.0f), "FT4 3.8 s, 3 s back: into its start");
    check(!ftx_sync_moment(FT4, 3.7f, 0.3f), "FT4 3.7 s: before the middle");
}

/* A radio whose right clock shows another median DT (clock_sync.c learns it under NTP): its checks go by that one */
static void learned_rx_dt(void) {
    ftx_sync_result_t r;
    band_t            b = {.proto = FT8, .clock = 0.0f, .start = 0.2f, .ofs = OFS};

    check(near(ftx_sync_rx_dt(), FTX_SYNC_RX_DT_S, 1e-6f), "the receive DT is FTX_SYNC_RX_DT_S until set");
    ftx_sync_reset(&sync);
    band(&b, 4, 7, right, 9);
    ftx_sync_set_rx_dt(0.45f);      /* this radio's right clock reads 0.45 s: by it ours is 0.3 s behind */
    check(look(&b, 4, &r) == FTX_SYNC_STEP && near(r.step, 0.30f, 0.02f), "learned 0.45 s: a step of +0.30 s");
    ftx_sync_set_rx_dt(0.20f);
    check(look(&b, 4, &r) == FTX_SYNC_ON_TIME, "learned 0.20 s: on time (0.05 s)");
    ftx_sync_set_rx_dt(FTX_SYNC_RX_DT_S);
}

int main(void) {
    right_and_off(FT8);
    right_and_off(FT4);
    enough();
    votes();
    clock_set();
    window();
    modes();
    dt();
    moment();
    learned_rx_dt();
    printf("%d of %d right\n", cases - bad, cases);
    return bad != 0;
}
