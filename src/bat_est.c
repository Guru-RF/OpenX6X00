/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 */

#include "bat_est.h"

#include <stdio.h>
#include <string.h>

#define NF          6
#define WIN         (20 * 60)   /* one learning step per 20 min on battery */
#define SETTLE      (10 * 60)   /* after the charger goes the base's % drops 5-10 points in a few minutes */
#define GAP         180
#define RING        64
#define SURE_DIS    6           /* 2 h of discharge seen before the "~" goes */
#define SURE_CHG    2           /* charges seen to the end from this 10 % */

/* Minutes the pack takes to go through each 10 % (0-10 ... 90-100) on receive, WiFi and BT on,
 * screen at 2: full discharges of 5, 6 and 17 Oct 2026, 0 % = the 6.0 V power-off. The base's %
 * comes from the voltage: not linear in time, 10-20 goes fastest */
static const float  band[10] = { 26, 18, 22, 26, 31, 27, 27, 28, 29, 22 };

/* The load against the one of band[]: x = 1, average TX watts, WiFi, Bluetooth, headphones,
 * brightness/10; weights in %/min */
static const float  w[NF] = { 0.42f, 0.25f, 0.04f, 0.03f, 0.03f, 0.02f };
static const float  x_ref[NF] = { 1, 0, 1, 1, 0, 0.2f };

/* Minutes from 0, 10 ... 90 % (the base's % while charging, 15-20 points above the one on battery)
 * until the charger stops at 95-99 %: charges of 6, 11, 13, 15 and 16 Oct 2026. Up to 50 % in
 * under an hour, then slow: 60 to 70 % takes about 2.5 h */
static const float  def_ttf[11] = { 490, 480, 478, 474, 470, 436, 391, 243, 155, 82, 0 };

static float    k;              /* learned: how much longer than band[] this pack lasts */
static int      n_dis;
static time_t   learned_t;      /* the log is replayed at every start: learn only what is newer */
static float    ttf[11];
static int      n_ttf[10];

static struct {
    time_t  t;
    float   cap;
    float   x[NF];
    bool    ext;
} ring[RING];
static int      ring_n, ring_head;

/* This charge: when each 10 % was first seen and at which % */
static struct {
    time_t  t;
    float   cap;
} seen[10];
static bool     charge_done;
static int      chg_off;        /* the base's charging flag drops for a reading now and then */

static bat_sample_t last;
static bool         have_last;
static time_t       win_start, stretch_start;
static float        shown;      /* minutes to empty, smoothed */

void bat_est_reset(void) {
    k = 1;
    n_dis = 0;
    learned_t = 0;
    memcpy(ttf, def_ttf, sizeof(ttf));
    memset(n_ttf, 0, sizeof(n_ttf));
    memset(seen, 0, sizeof(seen));
    ring_n = ring_head = 0;
    have_last = false;
}

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

static int bucket(float cap) {
    int b = (int) cap / 10;

    return b < 0 ? 0 : b > 9 ? 9 : b;
}

/* The load against the one of band[] */
static float load(const float *x) {
    float r = 0, r0 = 0;

    for (int i = 0; i < NF; i++) {
        r += w[i] * x[i];
        r0 += w[i] * x_ref[i];
    }
    return r / r0;
}

/* Minutes from hi % down to lo % at the load of band[] */
static float span(float hi, float lo) {
    float m = 0;

    for (int b = bucket(hi); b >= 0 && hi > lo; b--) {
        float l = b * 10 > lo ? b * 10 : lo;

        m += (hi - l) * band[b] / 10;
        hi = l;
    }
    return m;
}

/* Least squares slope of the % over [from, to] in %/min, the mean of x alongside */
static bool window(time_t from, time_t to, float *slope, float *xm, float *capm) {
    double  st = 0, sc = 0, stt = 0, stc = 0;
    int     n = 0;

    memset(xm, 0, NF * sizeof(float));
    for (int i_ = 0; i_ < ring_n; i_++) {
        int i = (ring_head - 1 - i_ + RING) % RING;

        if (ring[i].t < from) break;
        if (ring[i].t > to) continue;
        double t = (ring[i].t - from) / 60.0;
        st += t; sc += ring[i].cap; stt += t * t; stc += t * ring[i].cap;
        for (int j = 0; j < NF; j++) xm[j] += ring[i].x[j];
        n++;
    }
    if (n < 8) return false;
    for (int j = 0; j < NF; j++) xm[j] /= n;
    double d = n * stt - st * st;

    if (d <= 0) return false;
    *slope = (n * stc - st * sc) / d;
    *capm = sc / n;
    return true;
}

/* The minutes the window took against those band[] gives for the % it went through */
static void learn(time_t from, time_t to) {
    float slope, xm[NF], capm, mins = (to - from) / 60.0f;

    if (!window(from, to, &slope, xm, &capm) || slope >= 0) return;
    float ref = span(capm - slope * mins / 2, capm + slope * mins / 2);

    if (ref <= 0) return;
    /* One odd window (a jump of the base's %, TX in old rows without watts) moves little */
    float k_obs = clampf(mins * load(xm) / ref, 0.75f * k, 1.33f * k);

    k = clampf(k + 0.15f * (k_obs - k), 0.3f, 3);
    n_dis++;
}

/* Time to full at cap %: between the two 10 % points around it */
static float ttf_at(float cap, float *f) {
    int b = bucket(cap);

    *f = clampf((cap - b * 10) / 10, 0, 1);
    return ttf[b] * (1 - *f) + ttf[b + 1] * *f;
}

/* The charge got to its end: each 10 % seen tells how long it really took from there */
static void learn_charge(time_t end) {
    for (int b = 0; b < 10; b++) {
        if (!seen[b].t) continue;
        float f, pred = ttf_at(seen[b].cap, &f);
        float e = clampf((end - seen[b].t) / 60.0f - pred, -0.5f * pred - 10, 0.5f * pred + 10);

        ttf[b] = clampf(ttf[b] + 0.3f * e * (1 - f), 0, 1500);
        if (b < 9) ttf[b + 1] = clampf(ttf[b + 1] + 0.3f * e * f, 0, 1500);
        n_ttf[b]++;
    }
}

/* The charging flag stays on for a reading after the plug is pulled */
static bool is_ext(const bat_sample_t *s) {
    return s->vext >= 3;
}

/* Minutes to empty now: the median % of the last 5 readings (it jumps by 2-3 points), the load
 * of the last 20 min */
static float minutes_now(void) {
    float xm[NF] = { 0 }, c[5];
    int   n = 0;

    for (int i_ = 0; i_ < ring_n; i_++) {
        int i = (ring_head - 1 - i_ + RING) % RING;

        if (ring[i].ext || ring[i].t < last.t - WIN || ring[i].t < stretch_start) break;
        for (int j = 0; j < NF; j++) xm[j] += ring[i].x[j];
        if (n < 5) c[n] = ring[i].cap;
        n++;
    }
    for (int j = 0; j < NF; j++) xm[j] /= n;
    int m = n < 5 ? n : 5;

    for (int i = 1; i < m; i++) {
        for (int j = i; j > 0 && c[j] < c[j - 1]; j--) {
            float t = c[j]; c[j] = c[j - 1]; c[j - 1] = t;
        }
    }
    return span(c[m / 2], 0) * k / load(xm);
}

void bat_est_feed(const bat_sample_t *s) {
    bool ext = is_ext(s);
    float dt = have_last ? (float) (s->t - last.t) : 0;
    bool fresh = !have_last || dt <= 0 || dt > GAP || ext != is_ext(&last) || s->vbat < 3;

    if (fresh) {
        win_start = s->t + SETTLE;      /* a new stretch */
        stretch_start = s->t;
        memset(seen, 0, sizeof(seen));
        charge_done = false;
        chg_off = 0;
    }
    have_last = true;
    last = *s;
    if (s->vbat < 3) return;

    ring[ring_head].t = s->t;
    ring[ring_head].cap = s->cap;
    ring[ring_head].ext = ext;
    float *x = ring[ring_head].x;

    x[0] = 1;
    x[1] = dt > 0 ? s->tx_w * s->tx_s / dt : 0;
    x[2] = s->wifi;
    x[3] = s->bt;
    x[4] = s->hp;
    x[5] = s->bl > 0 ? s->bl / 10.0f : 0;
    ring_head = (ring_head + 1) % RING;
    if (ring_n < RING) ring_n++;

    if (ext) {
        chg_off = s->charging ? 0 : chg_off + 1;
        if (charge_done) return;
        /* The charger stops at 95-99 %; lower down the flag can stay off a few minutes */
        if (s->cap >= 99 || (chg_off >= 3 && s->cap >= 90)) {
            if (s->t > learned_t) {
                learn_charge(s->t);
                learned_t = s->t;
            }
            charge_done = true;
        } else if (s->t >= win_start && !seen[bucket(s->cap)].t) {
            seen[bucket(s->cap)].t = s->t;
            seen[bucket(s->cap)].cap = s->cap;
        }
        return;
    }
    if (s->t - win_start >= WIN) {
        if (s->t > learned_t) {
            learn(win_start, s->t);
            learned_t = s->t;
        }
        win_start = s->t;
    }

    /* A countdown that follows the model slowly: no jumps when the % does. Never above the model
     * though: near empty the base's % can fall 30 points in a few minutes, and the slow follow
     * kept hours that were not there (0 % and 3h52 left, issue #31). So down at once, up slowly;
     * at the base's 0 % nothing is left. Not on a reading with TX in its minute: the % sags under
     * the TX current (radio.c holds the shown one), and one 0 would show 0h00 next to 11 %. The
     * median of the last five gets there after three 0 readings */
    float now = s->cap > 0 || s->tx_s > 0 ? minutes_now() : 0;

    shown = fresh ? now : shown - dt / 60;
    shown = clampf(shown + 0.2f * (now - shown), 0, now);
}

int bat_est_minutes(int to_cap, bool *charging, bool *approx) {
    if (!have_last || last.vbat < 3) return -1;
    float cap = last.cap;

    if (is_ext(&last)) {
        /* Full, or the base says it is not charging (one reading off now and then below 90 %) */
        if (charge_done || chg_off >= 2 || (chg_off && cap >= 90)) return -1;
        float f;

        *charging = true;
        *approx = n_ttf[bucket(cap)] < SURE_CHG || last.t < stretch_start + SETTLE;  /* the % jumps up at first */
        return (int) (ttf_at(cap, &f) + 0.5f);
    }
    float min = shown;

    if (to_cap > 0) min -= span(to_cap, 0) * k;
    *charging = false;
    *approx = n_dis < SURE_DIS || last.t < stretch_start + SETTLE;
    return min < 0 ? 0 : (int) (min + 0.5f);
}

void bat_est_learned(int *hours, int *charges) {
    *hours = n_dis * WIN / 3600;
    *charges = 0;
    for (int b = 0; b < 10; b++) {
        if (n_ttf[b] > *charges) *charges = n_ttf[b];
    }
}

float bat_est_factor(void) {
    return k;
}

bool bat_est_save(const char *path) {
    char  tmp[256];
    FILE *f;

    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (!(f = fopen(tmp, "w"))) return false;
    fprintf(f, "{\"v\":3,\"n\":%d,\"k\":%.4f,\"lt\":%lld,\"ttf\":[", n_dis, k, (long long) learned_t);
    for (int b = 0; b < 10; b++) fprintf(f, "%s%.1f", b ? "," : "", ttf[b]);
    fputs("],\"nttf\":[", f);
    for (int b = 0; b < 10; b++) fprintf(f, "%s%d", b ? "," : "", n_ttf[b]);
    fputs("]}\n", f);
    bool ok = fclose(f) == 0;

    return ok && rename(tmp, path) == 0;
}

/* An older model (v2) is not read: the caller then learns again from the logs */
bool bat_est_load(const char *path) {
    FILE     *f = fopen(path, "r");
    char      s[512];
    float     kk, t[10];
    long long lt;
    int       n, nt[10];
    bool      ok = false;

    bat_est_reset();
    if (!f) return false;
    if (fgets(s, sizeof(s), f) &&
        sscanf(s, "{\"v\":3,\"n\":%d,\"k\":%f,\"lt\":%lld,\"ttf\":[%f,%f,%f,%f,%f,%f,%f,%f,%f,%f],"
                  "\"nttf\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d]}",
               &n, &kk, &lt, &t[0], &t[1], &t[2], &t[3], &t[4], &t[5], &t[6], &t[7], &t[8], &t[9],
               &nt[0], &nt[1], &nt[2], &nt[3], &nt[4], &nt[5], &nt[6], &nt[7], &nt[8], &nt[9]) == 23) {
        n_dis = n;
        k = clampf(kk, 0.3f, 3);
        learned_t = lt;
        for (int b = 0; b < 10; b++) {
            ttf[b] = clampf(t[b], 0, 1500);
            n_ttf[b] = nt[b];
        }
        ok = true;
    }
    fclose(f);
    return ok;
}

bool bat_est_replay(const char *path) {
    FILE *f = fopen(path, "r");
    char  line[160];

    if (!f) return false;
    while (fgets(line, sizeof(line), f)) {
        long         t;
        int          chg_flag, tx, wifi = 1, bt = 1, hp = 0;
        unsigned     mhz;
        bat_sample_t s = { 0 };

        int n = sscanf(line, "%ld,%f,%d,%f,%d,%d,%d,%u,%f,%f,%d,%d,%d", &t, &s.vbat, &s.cap, &s.vext,
                       &chg_flag, &tx, &s.bl, &mhz, &s.tx_s, &s.tx_w, &wifi, &bt, &hp);
        if (n < 7) continue;    /* the header */
        s.t = t;
        s.charging = chg_flag;
        s.wifi = wifi;          /* old rows (8 columns): taken as the usual WiFi and BT on */
        s.bt = bt;
        s.hp = hp;
        bat_est_feed(&s);       /* old rows: TX only as a flag, its power unknown */
    }
    fclose(f);
    return true;
}
