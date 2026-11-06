/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  The NCDXF/IARU beacons: schedule and scoring (beacons.h)
 *
 *  The schedule (ncdxf.org/beacon): 4U1UN starts the hour on 14.100 and the round, the next
 *  beacon takes 14.100 10 s later while 4U1UN goes up to 18.110, and so on: on band b in slot s
 *  (0..17) beacon (s - b) mod 18 is on the air.
 *
 *  A slot's score: its loud readings (the 90th percentile) over the same percentile of the
 *  band's noise. The noise is taken from the band's last slots: most beacons are not heard at
 *  any one time (or are off the air), so their slots are noise only; the lower quarter of those
 *  slots' loud readings is the noise's, and up to three slots in four may carry a beacon
 *  without lifting it. Loud over loud: the noise's own ups and downs do not count as a signal
 */

#include "beacons.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

const beacon_t beacons[BEACONS_N] = {
    {"4U1UN",  "New York, UN",        "FN30as"},
    {"VE8AT",  "Eureka, Canada",      "EQ79ax"},
    {"W6WX",   "California, USA",     "CM97bd"},
    {"KH6RS",  "Maui, Hawaii",        "BL10ts"},
    {"ZL6B",   "Masterton, NZ",       "RE78tw"},
    {"VK6RBP", "Perth, Australia",    "OF87av"},
    {"JA2IGY", "Mt Asama, Japan",     "PM84jk"},
    {"RR9O",   "Novosibirsk, Russia", "NO14kx"},
    {"VR2B",   "Hong Kong",           "OL72bg"},
    {"4S7B",   "Colombo, Sri Lanka",  "MJ96wv"},
    {"ZS6DN",  "Pretoria, S. Africa", "KG44dc"},
    {"5Z4B",   "Nairobi, Kenya",      "KI88ks"},
    {"4X6TU",  "Tel Aviv, Israel",    "KM72jb"},
    {"OH2B",   "Lohja, Finland",      "KP20dh"},
    {"CS3B",   "Madeira, Portugal",   "IM12or"},
    {"LU4AA",  "Buenos Aires, Arg.",  "GF05tj"},
    {"OA4B",   "Lima, Peru",          "FH17mw"},
    {"YV5B",   "Caracas, Venezuela",  "FJ69cc"},
};

const int32_t beacon_freq[BEACON_BANDS]      = {14100000, 18110000, 21150000, 24930000, 28200000};
const char   *beacon_band_name[BEACON_BANDS] = {"20m", "17m", "15m", "12m", "10m"};

#define SAMPLES_MAX   400       /* a slot's readings: 20 a second at most kept */
#define FLOOR_SLOTS   18        /* the band's last slots for its noise: a round */
#define HEARD_DB      6
#define HEARD_N       4         /* readings that loud, at least */
#define SETTLE_MS     500
#define MIN_SAMPLES   8
#define NOISE_SPREAD_DB 3.0f    /* the noise's 90th over its 20th percentile, about */

static int slot_of(time_t t) {
    return (int)((t % BEACON_ROUND_S + BEACON_ROUND_S) % BEACON_ROUND_S) / 10;
}

int beacon_on(int band, time_t t) {
    if (band < 0 || band >= BEACON_BANDS) return -1;
    return (slot_of(t) - band + BEACONS_N) % BEACONS_N;
}

int beacon_band_of(int beacon, time_t t) {
    int b = (slot_of(t) - beacon + BEACONS_N) % BEACONS_N;

    return b < BEACON_BANDS ? b : -1;
}

int beacon_band_at(int32_t freq) {
    for (int b = 0; b < BEACON_BANDS; b++)
        if (abs(freq - beacon_freq[b]) <= 500) return b;
    return -1;
}

const char *beacon_power_heard(int snr) {
    if (snr >= HEARD_DB + 30) return "0.1 W";
    if (snr >= HEARD_DB + 20) return "1 W";
    if (snr >= HEARD_DB + 10) return "10 W";
    return "100 W";
}

/* ---- Scoring ---- */

static beacon_heard_t heard[BEACONS_N][BEACON_BANDS];

#define BAND_DROPPED  -2      /* it changed band, a retune or TX in it: not scored */
#define BAND_PENDING  -3      /* retuned in its first SETTLE_MS: the band of its first reading */

static struct {
    int64_t slot;           /* ms / BEACON_SLOT_MS of the slot being read, -1 none */
    int     band;           /* the band read, or BAND_DROPPED, BAND_PENDING */
    float   db[SAMPLES_MAX];
    int     n;
    int64_t last_ms;
} cur;

static struct {
    float q[FLOOR_SLOTS];
    int   n, next;
} floors[BEACON_BANDS];

static int cmp_float(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;

    return (x > y) - (x < y);
}

static float pct(const float *sorted, int n, int p) {
    return sorted[(n - 1) * p / 100];
}

float beacons_floor(int band) {
    if (band < 0 || band >= BEACON_BANDS || floors[band].n < 3) return NAN;

    float s[FLOOR_SLOTS];

    memcpy(s, floors[band].q, sizeof(float) * floors[band].n);
    qsort(s, floors[band].n, sizeof(float), cmp_float);
    return s[(floors[band].n - 1) / 4];
}

static void score(void) {
    int band = cur.band;

    if (cur.slot < 0 || band < 0 || cur.n < MIN_SAMPLES) return;

    float s[SAMPLES_MAX];

    memcpy(s, cur.db, sizeof(float) * cur.n);
    qsort(s, cur.n, sizeof(float), cmp_float);

    float quiet = pct(s, cur.n, 20), loud = pct(s, cur.n, 90);

    floors[band].q[floors[band].next] = loud;
    floors[band].next                 = (floors[band].next + 1) % FLOOR_SLOTS;
    if (floors[band].n < FLOOR_SLOTS) floors[band].n++;

    /* Until the band has a few slots, the slot's own quiet part and a typical spread of the
     * noise stand for it */
    float noise = beacons_floor(band);

    if (isnan(noise)) noise = quiet + NOISE_SPREAD_DB;

    int over = 0;

    for (int i = 0; i < cur.n; i++)
        if (cur.db[i] >= noise + HEARD_DB) over++;   /* not one spike: a few readings that loud */

    time_t          start = (time_t)(cur.slot * BEACON_SLOT_MS / 1000);
    int             b     = beacon_on(band, start);
    float           snr   = loud - noise;
    beacon_heard_t *h     = &heard[b][band];

    h->when  = start;
    h->heard = snr >= HEARD_DB && over >= HEARD_N;
    h->snr   = (int8_t)fmaxf(-60.0f, fminf(99.0f, roundf(snr)));
}

void beacons_reset(void) {
    for (int i = 0; i < BEACONS_N; i++)
        for (int b = 0; b < BEACON_BANDS; b++) heard[i][b] = (beacon_heard_t){BEACON_NONE, false, 0};
    memset(floors, 0, sizeof(floors));
    cur.slot = -1;
    cur.n    = 0;
}

/* The slot of ms under way: the one before scored first, once it is over */
static void slot_begin(int64_t slot, int band) {
    score();
    cur.slot    = slot;
    cur.band    = band;
    cur.n       = 0;
    cur.last_ms = 0;
}

void beacons_retune(int64_t ms) {
    int64_t slot = ms / BEACON_SLOT_MS;

    /* At a slot's start (the Sweep's and Follow's retunes, the first tick of a TX) it was the slot just over that
     * was dropped, unscored: Follow never scored its beacon but on 10 m, a Sweep never the round's last slot. That
     * one is scored first now, and a retune in the new slot's first SETTLE_MS, which beacons_sample leaves out
     * anyway, keeps it: the band of its first reading */
    if (slot != cur.slot) slot_begin(slot, BAND_PENDING);
    if (ms % BEACON_SLOT_MS < SETTLE_MS && !cur.n) cur.band = BAND_PENDING;
    else cur.band = BAND_DROPPED;
}

void beacons_sample(int64_t ms, int band, float db) {
    int64_t slot = ms / BEACON_SLOT_MS;

    if (slot != cur.slot) slot_begin(slot, band);
    if (cur.band == BAND_PENDING) cur.band = band;
    if (band != cur.band) cur.band = BAND_DROPPED;
    if (cur.band < 0 || isnan(db)) return;
    if (ms % BEACON_SLOT_MS < SETTLE_MS || ms - cur.last_ms < 50) return;    /* 20 a second at most */
    if (cur.n < SAMPLES_MAX) cur.db[cur.n++] = db;
    cur.last_ms = ms;
}

const beacon_heard_t *beacons_heard(int beacon, int band) {
    if (beacon < 0 || beacon >= BEACONS_N || band < 0 || band >= BEACON_BANDS) return NULL;
    return &heard[beacon][band];
}
