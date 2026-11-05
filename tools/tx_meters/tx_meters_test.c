/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - host test of the TX meters' readings (src/tx_meters.c, #53): each transmission's start from 0. On the
 *  bench X6200 (4-5 November 2026, a dummy load, TCI's tx_sensors 5 a second) a 1 W TX right after an 8 W one read
 *  7.9 W as it keyed, before any of its audio, then 0.2 W rising to 0.9 W; 8 W right after 1 W read 1.0 W first. The
 *  readings were the last transmission's until the LVGL thread set them to 0 at the TX event, after the base's first
 *  TX packet; in the modes but the digital ones, the packets stored before that were smoothed down from them.
 *
 *  The readings go in as radio.c gives them in the radio thread: one a base packet in TX (tx_info_update; the X6200's
 *  base sends one about every 35 ms), tx_info_clear on the way out (tx_over). The bench's power steps are each held
 *  for six packets, about the 200 ms between two of TCI's readings. Checked: what a reader gets as the radio keys,
 *  before the base's first TX packet (TCI, CAT, the remote's Panel, the S-meter box: tx_info_get, tx_info_get_alc);
 *  the first reading as the base gave it, in CW (smoothed) and in the digital modes (each one); never the last
 *  transmission's power in the new one, both ways round; v0.3.8's way beside it. A high SWR shows on its first
 *  reading, and a matched TX after a mismatched one does not start red. tx_info_refresh tells of readings only, which
 *  tx_player and JS8's SWR guard wait for: the clear is none. The ALC greyed in CW, also asked without a pointer.
 *  See the Makefile.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

/* The views' job is the LVGL thread's: counted, not run, so it drops out of the link with the drawing (the Makefile) */
#define scheduler_put_coalesced(fn) views_job()

#include "tx_meters.c"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>

static int cases, good, jobs;

bool views_job(void) {
    jobs++;
    return true;
}

static void check(bool ok, const char *fmt, ...) {
    va_list ap;

    cases++;
    good += ok;
    printf("%s ", ok ? "  ok " : "  BAD");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

static bool near(float a, float b) {
    return fabsf(a - b) < 0.001f;
}

#define PER_STEP 6      /* packets a power step is held for: about 200 ms at the X6200's 35 ms */
#define N(a)     ((int)(sizeof(a) / sizeof((a)[0])))

/* The bench X6200 on 10 m, run 1 of each: TCI's readings from +200 ms on */
static const float after_8w[]  = {0.2f, 0.1f, 0.5f, 0.6f, 0.7f, 0.8f, 0.8f, 0.9f, 0.9f};  /* 1 W right after 8 W */
static const float after_1w[]  = {1.7f, 1.3f, 3.9f, 4.7f, 5.5f, 6.4f, 6.7f, 7.1f, 7.3f};  /* 8 W right after 1 W */
static const float steady_8w[] = {8.0f, 8.0f, 8.0f, 8.0f, 8.0f, 8.0f};
static const float steady_1w[] = {0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f};

typedef struct {
    float key_w, key_swr, key_alc;  /* as the radio keys, before the base's first TX packet */
    float first_w, first_swr;       /* after the first reading */
    float top_w, top_swr;           /* the most shown, from the key on */
} seen_t;

/* One transmission as radio.c gives it: the power steps, PER_STEP packets each, the SWR and the ALC steady; then
 * tx_over's tx_info_clear, or none (v0.3.8's way: TCI read before the LVGL thread set them to 0) */
static seen_t transmit(const float *w, int steps, float swr, float alc, bool clear) {
    seen_t s;

    tx_info_get(&s.key_w, &s.key_swr);
    s.key_alc = tx_info_get_alc(NULL);
    s.top_w   = s.key_w;
    s.top_swr = s.key_swr;
    for (int i = 0; i < steps * PER_STEP; i++) {
        float p, r;

        tx_info_update(w[i / PER_STEP], swr, alc);
        tx_info_get(&p, &r);
        if (i == 0) {
            s.first_w   = p;
            s.first_swr = r;
        }
        s.top_w   = LV_MAX(s.top_w, p);
        s.top_swr = LV_MAX(s.top_swr, r);
    }
    if (clear) {
        tx_info_clear();
    }
    return s;
}

/* Low after high and high after low, in a mode */
static void both_ways(const char *name, radio_mode_t mode) {
    seen_t s;
    float  p, r;

    cur_mode = mode;
    transmit(steady_8w, N(steady_8w), 1.2f, 5.0f, true);
    tx_info_get(&p, &r);
    check(p == 0.0f && r == 0.0f && tx_info_get_alc(NULL) == 0.0f, "%s: back in RX after 8 W, power, SWR and ALC 0",
          name);

    s = transmit(after_8w, N(after_8w), 1.2f, 5.0f, true);
    check(s.key_w == 0.0f && s.key_swr == 0.0f && s.key_alc == 0.0f,
          "%s, 1 W right after 8 W: as it keys %.1f W, SWR %.1f, ALC %.1f (the bench read 7.9 W)", name, s.key_w,
          s.key_swr, s.key_alc);
    check(near(s.first_w, 0.2f), "%s, 1 W right after 8 W: the first reading %.2f W, as the base gave it (0.2)", name,
          s.first_w);
    check(s.top_w <= 0.9f + 0.001f, "%s, 1 W right after 8 W: %.2f W at most, the base's 0.9 W", name, s.top_w);

    s = transmit(after_1w, N(after_1w), 1.2f, 5.0f, true);
    check(s.key_w == 0.0f, "%s, 8 W right after 1 W: as it keys %.1f W (the bench read 1.0 W)", name, s.key_w);
    check(near(s.first_w, 1.7f), "%s, 8 W right after 1 W: the first reading %.2f W, as the base gave it (1.7)", name,
          s.first_w);
    check(s.top_w <= 7.3f + 0.001f, "%s, 8 W right after 1 W: %.2f W at most, the base's 7.3 W", name, s.top_w);
}

int main(void) {
    seen_t  s;
    float   p, r, a = -1.0f;
    uint8_t seen = 0, reading = 0;
    bool    grey = false, fresh;
    int     before;

    /* At the start, before any TX */
    tx_info_get(&p, &r);
    check(p == 0.0f && r == 0.0f && tx_info_get_alc(NULL) == 0.0f, "before any TX: power, SWR and ALC 0");
    check(!tx_info_refresh(&seen, NULL, NULL, NULL), "before any TX: no reading to tell of");

    both_ways("CW", radio_mode_cw);
    both_ways("USB-D", radio_mode_usb_dig);

    /* v0.3.8's way: no clear in the radio thread, so what a reader in another thread got as the radio keyed */
    cur_mode = radio_mode_cw;
    transmit(steady_8w, N(steady_8w), 1.2f, 5.0f, false);
    s = transmit(after_8w, N(after_8w), 1.2f, 5.0f, false);
    check(s.key_w > 7.9f, "v0.3.8's way, 1 W right after 8 W: %.1f W as it keyed (TCI on the bench: 7.9 W)", s.key_w);
    check(s.first_w > 7.0f && s.top_w > 7.9f,
          "v0.3.8's way, the LVGL thread's 0 after the first packets: CW's first reading %.1f W, smoothed from 8 W",
          s.first_w);
    tx_info_clear();

    /* The SWR: a high one shows at once, and does not colour the next TX's start */
    cur_mode = radio_mode_cw;
    transmit(steady_1w, N(steady_1w), 1.2f, 5.0f, true);
    s = transmit(steady_1w, N(steady_1w), 3.4f, 5.0f, true);
    check(s.key_swr == 0.0f && near(s.first_swr, 3.4f),
          "CW, SWR 3.4 right after 1.2: as it keys %.1f, the first reading %.2f, not smoothed up from 1.2", s.key_swr,
          s.first_swr);
    s = transmit(steady_1w, N(steady_1w), 1.2f, 5.0f, true);
    check(s.top_swr < 2.0f, "CW, SWR 1.2 right after 3.4: %.2f at most, not yellow or red from the last TX", s.top_swr);
    s = transmit(steady_1w, N(steady_1w), 7.5f, 5.0f, true);
    check(near(s.first_swr, 5.0f), "CW, SWR 7.5: the first reading %.1f, the top of the scale", s.first_swr);

    /* tx_info_refresh tells of readings only: tx_player at its key, JS8's SWR guard while not keyed */
    cur_mode = radio_mode_usb_dig;
    transmit(steady_8w, 1, 1.2f, 5.0f, false);
    tx_info_refresh(&reading, NULL, NULL, NULL);
    tx_info_clear();
    check(!tx_info_refresh(&reading, &a, &p, &r), "tx_player: the clear is no reading");
    tx_info_update(0.2f, 1.3f, 0.4f);
    fresh = tx_info_refresh(&reading, &a, &p, &r);
    check(fresh && near(p, 0.2f) && near(r, 1.3f) && near(a, 0.4f),
          "tx_player: the first reading is one, as the base gave it: %.1f W, SWR %.1f, ALC %.1f", p, r, a);
    check(!tx_info_refresh(&reading, NULL, NULL, NULL), "tx_player: and only once");
    tx_info_clear();

    /* The LVGL thread: a reading asks the views to show it; the clear asks nothing (RX: the views are hidden) */
    before = jobs;
    tx_info_clear();
    check(jobs == before, "the clear asks nothing of the LVGL thread");
    tx_info_update(1.0f, 1.2f, 0.0f);
    check(jobs == before + 1, "a reading asks the views to show it");
    tx_info_clear();

    /* The ALC greyed in CW, and asked without a pointer (TCI, CAT, the remote) */
    cur_mode = radio_mode_cw;
    tx_info_get_alc(&grey);
    check(grey, "CW: the ALC greyed");
    cur_mode = radio_mode_usb;
    tx_info_get_alc(&grey);
    check(!grey, "USB: the ALC not greyed");

    printf("TX_METERS %s: %d of %d cases\n", good == cases ? "OK" : "FAILED", good, cases);
    return good == cases ? 0 : 1;
}
