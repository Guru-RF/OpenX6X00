/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - a time station's minute marker (time_station.h).
 *
 *  The tone's amplitude over 10 ms (a boxcar: a tone 200 Hz away, WWVH's 1200 Hz to WWV's 1000, falls in its second
 *  null), a value each ms timed at the boxcar's middle. A run over three times the noise (the median of the last 3 s:
 *  the pulses take a tenth to a third of the time) as long as the marker, give or take 100 ms, is one; the second
 *  pulses are too short (RWM 100 ms, CHU 300 ms, WWV 5 ms). Its start is where the tone crosses halfway from the
 *  noise to its level. The audio's time comes from the clock at each block of samples: the earliest of the last 10 to
 *  20 s, as a block comes late but never early.
 *
 *  From OlivLoze's fork (F4OLI), its clock_beacon.c.
 */

#include "time_station.h"

#include <complex.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FS          TIME_STATION_FS
#define BOX         110         /* 10 ms */
#define ENV_N       4096        /* ms of the amplitude kept */
#define FLOOR_N     3000        /* the noise: the median of 3 s */
#define FLOOR_EVERY 250
#define BASE_BLOCK  10.0        /* s */
#define MARKER_TOL  100         /* ms */
#define GAP_TOL     0.05        /* s: two markers whole minutes apart */
#define AGREE       0.03        /* s: and saying the same */

const time_station_t time_stations[] = {
    {"RWM 4996", 4996000, 4995000, 1000, 500, true},
    {"RWM 9996", 9996000, 9995000, 1000, 500, true},
    {"RWM 14996", 14996000, 14995000, 1000, 500, true},
    {"WWV 2500", 2500000, 2500000, 1000, 800, false},
    {"WWV 5000", 5000000, 5000000, 1000, 800, false},
    {"WWV 10000", 10000000, 10000000, 1000, 800, false},
    {"WWV 15000", 15000000, 15000000, 1000, 800, false},
    {"WWV 20000", 20000000, 20000000, 1000, 800, false},
    {"WWVH 5000", 5000000, 5000000, 1200, 800, false},
    {"WWVH 10000", 10000000, 10000000, 1200, 800, false},
    {"WWVH 15000", 15000000, 15000000, 1200, 800, false},
    {"CHU 3330", 3330000, 3330000, 1000, 500, false},
    {"CHU 7850", 7850000, 7850000, 1000, 500, false},
    {"CHU 14670", 14670000, 14670000, 1000, 500, false},
};
const int time_stations_n = sizeof(time_stations) / sizeof(time_stations[0]);

static pthread_mutex_t      lock = PTHREAD_MUTEX_INITIALIZER;
static time_station_state_t pub;
static volatile bool        listening;
static volatile bool        restart;

/* ---- audio thread ---- */

static uint64_t idx;                    /* samples since the start */
static double   base_cur, base_prev;    /* the clock of sample 0, the two blocks' earliest */
static double   block_t0;

static double complex osc, rot;
static double complex box[BOX], box_sum;
static int            box_pos;
static double         ms_acc;

static float    env[ENV_N];
static double   env_t[ENV_N];
static uint64_t env_k;                  /* values so far */
static float    noise;
static bool     on;
static uint64_t on_k;
static int      marker_ms;

static double   prev_t = NAN, prev_raw;

static void reset(int station) {
    idx       = 0;
    base_cur  = base_prev = INFINITY;
    block_t0  = NAN;
    osc       = 1.0;
    rot       = cexp(I * 2.0 * M_PI * time_stations[station].tone_hz / FS);
    marker_ms = time_stations[station].marker_ms;
    memset(box, 0, sizeof(box));
    box_sum   = 0;
    box_pos   = 0;
    ms_acc    = 0;
    env_k     = 0;
    noise     = 0;
    on        = false;
    prev_t    = NAN;
}

static int cmp_float(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;

    return (x > y) - (x < y);
}

static float at(uint64_t k) {
    return env[k % ENV_N];
}

/* A run of the tone from on_k to k, maybe the marker */
static void run_over(uint64_t k) {
    uint64_t len = k - on_k;

    if (len + MARKER_TOL < (uint64_t)marker_ms || len > (uint64_t)marker_ms + MARKER_TOL || on_k < 40 ||
        k - on_k + 40 >= ENV_N)
        return;

    float lvl[1000];
    int   n = 0;

    for (uint64_t i = on_k + 20; i + 20 < k && n < 1000; i++)
        lvl[n++] = at(i);
    if (n < 10)
        return;
    qsort(lvl, n, sizeof(float), cmp_float);

    float    level = lvl[n / 2], half = (noise + level) / 2;
    uint64_t edge  = on_k;

    for (uint64_t i = on_k - 30; i <= on_k + 30; i++)
        if (at(i) >= half) {
            edge = i;
            break;
        }

    double t   = env_t[edge % ENV_N];
    double raw = round(t / 60.0) * 60.0 - t;
    float  snr = 20.0f * log10f(level / fmaxf(noise, 1e-9f));

    pthread_mutex_lock(&lock);
    pub.snr_db    = snr;
    pub.markers++;
    pub.marker_at = t;
    if (isfinite(prev_t)) {
        double gap = t - prev_t;
        double m   = round(gap / 60.0);

        if (m >= 1 && m <= 5 && fabs(gap - 60.0 * m) < GAP_TOL && fabs(raw - prev_raw) < AGREE) {
            pub.raw    = (float)((raw + prev_raw) / 2);
            pub.result = true;
            pub.result_id++;
        }
    }
    pthread_mutex_unlock(&lock);
    prev_t   = t;
    prev_raw = raw;
}

static void put_env(float a, double t) {
    env[env_k % ENV_N]   = a;
    env_t[env_k % ENV_N] = t;
    env_k++;

    if (env_k % FLOOR_EVERY == 0 && env_k >= FLOOR_N) {
        static float v[FLOOR_N];

        for (int i = 0; i < FLOOR_N; i++)
            v[i] = at(env_k - FLOOR_N + i);
        qsort(v, FLOOR_N, sizeof(float), cmp_float);
        noise = v[FLOOR_N / 2];
    }
    if (noise <= 0)
        return;
    if (!on && a > 3.0f * noise) {
        on   = true;
        on_k = env_k - 1;
    } else if (on && a < 2.0f * noise) {
        on = false;
        run_over(env_k - 1);
    }
}

void time_station_audio_at(unsigned int n, const float *samples, double now) {
    if (!listening)
        return;
    if (restart) {
        restart = false;
        pthread_mutex_lock(&lock);
        int station = pub.station;
        pub.markers = 0;
        pub.result  = false;
        pthread_mutex_unlock(&lock);
        reset(station);
    }

    double cand = now - (idx + n) / FS;

    if (!isfinite(block_t0) || now - block_t0 > BASE_BLOCK) {
        base_prev = base_cur;
        base_cur  = INFINITY;
        block_t0  = now;
    }
    if (cand < base_cur)
        base_cur = cand;

    double base = fmin(base_cur, base_prev);

    for (unsigned int i = 0; i < n; i++, idx++) {
        double complex v = samples[i] * osc;

        osc *= rot;
        box_sum += v - box[box_pos];
        box[box_pos] = v;
        box_pos      = (box_pos + 1) % BOX;

        ms_acc += 1000.0;
        if (ms_acc >= FS) {
            ms_acc -= FS;
            if ((idx & 0xfff) == 0)
                osc /= cabs(osc);       /* the oscillator kept on the unit circle */
            if (idx >= BOX)
                put_env((float)(2.0 * cabs(box_sum) / BOX), base + (idx - BOX / 2.0) / FS);
        }
    }
    if ((idx & 0x3ffff) < n) {      /* the running sum summed again: no rounding left to pile up */
        box_sum = 0;
        for (int i = 0; i < BOX; i++)
            box_sum += box[i];
    }
}

void time_station_audio(unsigned int n, float *samples) {
    struct timespec ts;

    if (!listening)
        return;
    clock_gettime(CLOCK_REALTIME, &ts);
    time_station_audio_at(n, samples, ts.tv_sec + ts.tv_nsec / 1e9);
}

/* ---- any thread ---- */

void time_station_start(int i) {
    pthread_mutex_lock(&lock);
    memset(&pub, 0, sizeof(pub));
    pub.on      = true;
    pub.station = i >= 0 && i < time_stations_n ? i : 0;
    pthread_mutex_unlock(&lock);
    restart   = true;
    listening = true;
}

void time_station_stop(void) {
    listening = false;
    pthread_mutex_lock(&lock);
    pub.on = false;
    pthread_mutex_unlock(&lock);
}

void time_station_restart(void) {
    restart = true;
}

void time_station_get(time_station_state_t *st) {
    pthread_mutex_lock(&lock);
    *st = pub;
    pthread_mutex_unlock(&lock);
}

int time_station_quiet_until(const time_station_t *s, int minute) {
    if (!s || !s->quiet || minute % 30 >= 10)
        return -1;
    return minute < 30 ? 10 : 40;
}
