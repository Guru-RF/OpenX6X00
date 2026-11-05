/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  The scanner's engine (scan.h)
 */

#include "scan.h"

#include <stdlib.h>
#include <string.h>

#define HOLD_HYST_DB  3.0f      /* once stopped, it stays busy down to this much under the threshold */
#define QUIET_RISE_DB 0.5f      /* the quiet level let up on every visit */
#define WAIT_MAX      3         /* dwells to wait for a first reading (a slow S-meter) */
#define KNOWN_MIN     3         /* quiet levels known before a first visit counts on their median */

static struct {
    int          n, cur, passes;
    int          dwell_ms, hang_ms;
    float        stop_db, margin_db;
    scan_state_t state;
    int64_t      tuned_ms, active_ms;
    double       sum;           /* the readings of this look */
    int          got;
    bool         tune_due;
    float        quiet[SCAN_MAX];
    bool         locked[SCAN_MAX];
    scan_hit_t   hits[SCAN_HITS_MAX];
    int          hits_n;
    int64_t      hit_ms;
} s = {.cur = -1, .dwell_ms = 250, .hang_ms = 3000, .stop_db = NAN, .margin_db = 10.0f};

void scan_init(int n) {
    s.n      = n < 0 ? 0 : n > SCAN_MAX ? SCAN_MAX : n;
    s.cur    = -1;
    s.state  = SCAN_STOPPED;
    s.passes = 0;
    s.hits_n = 0;
    for (int i = 0; i < SCAN_MAX; i++) {
        s.quiet[i]  = NAN;
        s.locked[i] = false;
    }
}

void scan_config(int dwell_ms, int hang_ms, float stop_db, float margin_db) {
    s.dwell_ms  = dwell_ms < 60 ? 60 : dwell_ms;
    s.hang_ms   = hang_ms < 0 ? 0 : hang_ms;
    s.stop_db   = stop_db;
    s.margin_db = margin_db;
}

static int cmp_float(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;

    return (x > y) - (x < y);
}

/* The median of the quiet levels known, NAN with fewer than KNOWN_MIN */
static float quiet_median(void) {
    static float v[SCAN_MAX];
    int          k = 0;

    for (int i = 0; i < s.n; i++)
        if (!isnan(s.quiet[i]) && !s.locked[i]) v[k++] = s.quiet[i];
    if (k < KNOWN_MIN) return NAN;
    qsort(v, k, sizeof(float), cmp_float);
    return v[k / 2];
}

float scan_quiet(int i) {
    return i >= 0 && i < s.n ? s.quiet[i] : NAN;
}

float scan_threshold(int i) {
    if (i < 0 || i >= s.n) return NAN;
    if (!isnan(s.stop_db)) return s.stop_db;

    float q = isnan(s.quiet[i]) ? quiet_median() : s.quiet[i];

    return isnan(q) ? NAN : q + s.margin_db;
}

/* The next one from i on that is not locked out, -1 when they all are */
static int next_from(int i) {
    for (int k = 0; k < s.n; k++) {
        int j = ((i + k) % s.n + s.n) % s.n;

        if (!s.locked[j]) return j;
    }
    return -1;
}

static void tune(int i, int64_t now) {
    if (i < 0) {
        s.state = SCAN_STOPPED;
        s.cur   = -1;
        return;
    }
    if (s.cur >= 0 && i <= s.cur) s.passes++;
    s.cur      = i;
    s.tuned_ms = now;
    s.sum      = 0;
    s.got      = 0;
    s.tune_due = true;
}

void scan_start(int i, int64_t now_ms) {
    if (!s.n) return;
    s.state = SCAN_SEEKING;
    s.cur   = -1;
    tune(next_from(i), now_ms);
}

void scan_stop(void) {
    s.state = SCAN_STOPPED;
}

void scan_pause(bool on, int64_t now_ms) {
    if (on && (s.state == SCAN_SEEKING || s.state == SCAN_HOLD)) {
        s.state = SCAN_PAUSED;
    } else if (!on && s.state == SCAN_PAUSED) {
        s.state = SCAN_SEEKING;
        tune(next_from(s.cur + 1), now_ms);
    }
}

void scan_skip(int64_t now_ms) {
    if (s.state == SCAN_STOPPED || !s.n) return;
    s.state = SCAN_SEEKING;
    tune(next_from(s.cur + 1), now_ms);
}

static void hit_begin(int i, time_t wall, float db, int64_t now) {
    memmove(&s.hits[1], &s.hits[0], sizeof(scan_hit_t) * (SCAN_HITS_MAX - 1));
    s.hits[0] = (scan_hit_t){i, wall, 0, db};
    if (s.hits_n < SCAN_HITS_MAX) s.hits_n++;
    s.hit_ms = now;
}

void scan_reading(int64_t now_ms, float db) {
    if (isnan(db) || s.cur < 0) return;

    if (s.state == SCAN_SEEKING) {
        if (now_ms - s.tuned_ms < s.dwell_ms / 2) return;    /* still settling */
        s.sum += db;
        s.got++;
    } else if (s.state == SCAN_HOLD || s.state == SCAN_PAUSED) {
        float thr = scan_threshold(s.cur);

        if (!isnan(thr) && db >= thr - HOLD_HYST_DB) s.active_ms = now_ms;
        if (s.state == SCAN_HOLD && s.hits_n && s.hits[0].idx == s.cur) {
            if (db > s.hits[0].peak) s.hits[0].peak = db;
            s.hits[0].secs = (int)((s.active_ms - s.hit_ms) / 1000);
        }
    }
}

int scan_tick(int64_t now_ms, time_t wall) {
    if (s.tune_due) {
        s.tune_due = false;
        return s.cur;
    }
    if (s.state == SCAN_HOLD) {
        if (now_ms - s.active_ms > s.hang_ms) {
            s.state = SCAN_SEEKING;
            tune(next_from(s.cur + 1), now_ms);
            if (s.tune_due) {
                s.tune_due = false;
                return s.cur;
            }
        }
        return -1;
    }
    if (s.state != SCAN_SEEKING || s.cur < 0) return -1;

    int64_t age = now_ms - s.tuned_ms;

    if (age < s.dwell_ms || (!s.got && age < (int64_t)s.dwell_ms * WAIT_MAX)) return -1;

    int i = s.cur;

    if (s.got) {
        float level = (float)(s.sum / s.got);
        float thr   = scan_threshold(i);

        if (!isnan(thr) && level >= thr) {
            s.state     = SCAN_HOLD;
            s.active_ms = now_ms;
            hit_begin(i, wall, level, now_ms);
            return -1;
        }
        if (isnan(s.quiet[i]) || level < s.quiet[i]) s.quiet[i] = level;
        else s.quiet[i] = fminf(level, s.quiet[i] + QUIET_RISE_DB);
    }
    tune(next_from(i + 1), now_ms);
    if (s.tune_due) {
        s.tune_due = false;
        return s.cur;
    }
    return -1;
}

void scan_lockout(int i, bool on) {
    if (i >= 0 && i < s.n) s.locked[i] = on;
}

bool scan_locked(int i) {
    return i >= 0 && i < s.n && s.locked[i];
}

scan_state_t scan_state(void) {
    return s.state;
}

int scan_current(void) {
    return s.cur;
}

int scan_passes(void) {
    return s.passes;
}

int scan_hits_n(void) {
    return s.hits_n;
}

const scan_hit_t *scan_hit(int k) {
    return k >= 0 && k < s.hits_n ? &s.hits[k] : NULL;
}
