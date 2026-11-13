/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  The TX ranges (tx_ranges.h, #247): South Korea's table, the own list read from its file, and the check that puts
 *  them in front of the amateur bands' (ham_bands.c).
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "tx_ranges.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

/* South Korea's amateur ranges, Hz, both edges in, sorted, none touching: the table airis01 sent in #247 for the
 * station inspection of a radio bought abroad. Each lies inside a band of ham_bands.c (tools/tx_ranges checks both),
 * so TX outside ham bands changes nothing here. No 60 m, 8 m, 630 m or 2200 m: they are not on it */
static const struct {
    int32_t lo, hi;
} kr[] = {
    {1800000, 1825000},     /* 160 m */
    {3500000, 3550000},     /* 80 m */
    {3790000, 3800000},     /* 80 m */
    {7000000, 7200000},     /* 40 m */
    {10100000, 10150000},   /* 30 m */
    {14000000, 14350000},   /* 20 m */
    {18068000, 18168000},   /* 17 m */
    {21000000, 21450000},   /* 15 m */
    {24890000, 24990000},   /* 12 m */
    {28000000, 29700000},   /* 10 m */
    {50000000, 54000000},   /* 6 m */
};

#define KR_N ((int)(sizeof(kr) / sizeof(kr[0])))

/* The own list in use: sorted, none overlapping or touching. Under own_mux: a read of the file replaces it on one
 * thread while the others check a frequency against it */
static pthread_mutex_t  own_mux = PTHREAD_MUTEX_INITIALIZER;
static int32_t          own_lo[TX_RANGES_OWN_MAX], own_hi[TX_RANGES_OWN_MAX];
static int              own_n;
static tx_ranges_own_t  own_state = {.missing = true};

/* A list being read, before it goes in use */
typedef struct {
    int32_t         lo[TX_RANGES_OWN_MAX], hi[TX_RANGES_OWN_MAX];
    int             n;
    tx_ranges_own_t st;
} build_t;

static const char *skip(const char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    return s;
}

/* Nothing more on the line: its end, or a comment */
static bool line_end(const char *s) {
    return *s == '\0' || *s == '#';
}

/* A number of MHz at s, up to 6 decimals, into Hz: the text after it, NULL when it is none. Counted in whole numbers,
 * not read as a double: 7.2005 is 7200500 Hz, not 7200499 */
static const char *mhz(const char *s, int32_t *hz) {
    int64_t whole = 0, frac = 0;
    int     digits = 0, decimals = 0;

    for (; *s >= '0' && *s <= '9'; s++, digits++) {
        whole = whole * 10 + (*s - '0');
        if (whole > 1000) return NULL;      /* far past the radio's 55 MHz: kHz or Hz written, or a slip */
    }
    if (*s == '.') {
        for (s++; *s >= '0' && *s <= '9'; s++) {
            if (++decimals > 6) return NULL;    /* under 1 Hz */
            frac = frac * 10 + (*s - '0');
        }
        if (!decimals) return NULL;         /* "7." */
    }
    if (!digits) return NULL;
    for (; decimals < 6; decimals++) frac *= 10;
    *hz = (int32_t)(whole * 1000000 + frac);
    return s;
}

int tx_ranges_line(const char *line, int32_t *lo, int32_t *hi) {
    const char *s = skip(line);
    int32_t     a, b;

    if (line_end(s)) return 0;
    if (!(s = mhz(s, &a))) return -1;
    s = skip(s);
    if (*s != '-') return -1;
    if (!(s = mhz(skip(s + 1), &b))) return -1;
    if (!line_end(skip(s))) return -1;
    if (a > b || a < TX_RANGES_MIN_HZ || b > TX_RANGES_MAX_HZ) return -1;
    *lo = a;
    *hi = b;
    return 1;
}

static void build_bad(build_t *b, int no) {
    b->st.bad++;
    if (!b->st.first_bad) b->st.first_bad = no;
}

static void build_line(build_t *b, const char *line, int no) {
    int32_t lo, hi;
    int     r = tx_ranges_line(line, &lo, &hi);

    if (r == 0) return;
    if (r < 0 || b->n == TX_RANGES_OWN_MAX) {
        build_bad(b, no);   /* left out: a line not read only ever narrows the list */
        return;
    }
    b->lo[b->n] = lo;
    b->hi[b->n] = hi;
    b->n++;
}

/* Sorted, those that overlap or touch joined, and in use from now on */
static tx_ranges_own_t build_use(build_t *b) {
    int n = 0;

    for (int i = 1; i < b->n; i++) {    /* 64 at most: an insertion sort */
        int32_t lo = b->lo[i], hi = b->hi[i];
        int     j  = i;

        for (; j > 0 && b->lo[j - 1] > lo; j--) {
            b->lo[j] = b->lo[j - 1];
            b->hi[j] = b->hi[j - 1];
        }
        b->lo[j] = lo;
        b->hi[j] = hi;
    }
    for (int i = 0; i < b->n; i++) {
        if (n && b->lo[i] <= b->hi[n - 1] + 1) {
            if (b->hi[i] > b->hi[n - 1]) b->hi[n - 1] = b->hi[i];
        } else {
            b->lo[n] = b->lo[i];
            b->hi[n] = b->hi[i];
            n++;
        }
    }
    b->st.count = n;
    pthread_mutex_lock(&own_mux);
    memcpy(own_lo, b->lo, sizeof(own_lo));
    memcpy(own_hi, b->hi, sizeof(own_hi));
    own_n     = n;
    own_state = b->st;
    pthread_mutex_unlock(&own_mux);
    return b->st;
}

tx_ranges_own_t tx_ranges_own_set(const char *text) {
    build_t     b  = {0};
    const char *p  = text ? text : "";
    int         no = 0;

    while (*p) {
        const char *nl  = strchr(p, '\n');
        size_t      len = nl ? (size_t)(nl - p) : strlen(p);
        char        line[256];

        no++;
        if (len >= sizeof(line)) {
            build_bad(&b, no);
        } else {
            memcpy(line, p, len);
            line[len] = '\0';
            build_line(&b, line, no);
        }
        p += len + (nl ? 1 : 0);
    }
    return build_use(&b);
}

tx_ranges_own_t tx_ranges_own_load(const char *path) {
    build_t b  = {0};
    FILE   *f  = fopen(path, "r");
    char    line[256];
    int     no = 0;

    if (!f) {
        b.st.missing = true;
        return build_use(&b);
    }
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);

        no++;
        if (len == sizeof(line) - 1 && line[len - 1] != '\n') {    /* longer than any range: the rest of it skipped */
            int c;

            while ((c = fgetc(f)) != EOF && c != '\n') {}
            build_bad(&b, no);
            continue;
        }
        build_line(&b, line, no);
    }
    fclose(f);
    return build_use(&b);
}

tx_ranges_own_t tx_ranges_own_state(void) {
    tx_ranges_own_t st;

    pthread_mutex_lock(&own_mux);
    st = own_state;
    pthread_mutex_unlock(&own_mux);
    return st;
}

bool tx_ranges_own_get(int i, int32_t *lo, int32_t *hi) {
    bool ok;

    pthread_mutex_lock(&own_mux);
    ok = i >= 0 && i < own_n;
    if (ok) {
        *lo = own_lo[i];
        *hi = own_hi[i];
    }
    pthread_mutex_unlock(&own_mux);
    return ok;
}

/* The own list's range hz lies in: 1 found, 0 none, -1 the list is empty. One look under the lock, so an empty list
 * is told apart from a frequency outside it even while a read replaces the list */
static int own_find(int32_t hz, int32_t *lo, int32_t *hi) {
    int r = 0;

    pthread_mutex_lock(&own_mux);
    if (!own_n) {
        r = -1;
    }
    for (int i = 0; i < own_n && !r; i++) {
        if (hz >= own_lo[i] && hz <= own_hi[i]) {
            if (lo) *lo = own_lo[i];
            if (hi) *hi = own_hi[i];
            r = 1;
        }
    }
    pthread_mutex_unlock(&own_mux);
    return r;
}

static bool kr_find(int32_t hz, int32_t *lo, int32_t *hi) {
    for (int i = 0; i < KR_N; i++) {
        if (hz >= kr[i].lo && hz <= kr[i].hi) {
            if (lo) *lo = kr[i].lo;
            if (hi) *hi = kr[i].hi;
            return true;
        }
    }
    return false;
}

bool tx_ranges_of(tx_ranges_t p, int32_t hz, int32_t *lo, int32_t *hi) {
    switch (p) {
        case TX_RANGES_KR:
            return kr_find(hz, lo, hi);
        case TX_RANGES_OWN:
            return own_find(hz, lo, hi) > 0;
        default:
            return ham_band_of(hz, lo, hi);
    }
}

ham_tx_t tx_ranges_check(tx_ranges_t p, int32_t hz, bool hw_tx, int32_t shift, bool any) {
    if (hw_tx && p == TX_RANGES_KR && !kr_find(hz - shift, NULL, NULL)) {
        return HAM_TX_OFF_RANGE;
    }
    if (hw_tx && p == TX_RANGES_OWN) {
        switch (own_find(hz - shift, NULL, NULL)) {
            case -1: return HAM_TX_NO_RANGES;
            case 0:  return HAM_TX_OFF_RANGE;
            default: break;
        }
    }
    return ham_tx_check(hz, hw_tx, shift != 0, any);
}

bool tx_ranges_narrow(tx_ranges_t p, int32_t freq, int32_t shift, int32_t *lo, int32_t *hi) {
    int32_t rlo, rhi;

    if (p != TX_RANGES_KR && p != TX_RANGES_OWN) {
        return true;
    }
    if (!tx_ranges_of(p, freq - shift, &rlo, &rhi)) {
        return false;
    }
    if (rlo + shift > *lo) *lo = rlo + shift;
    if (rhi + shift < *hi) *hi = rhi + shift;
    return true;
}

const char *tx_ranges_why(ham_tx_t why) {
    switch (why) {
        case HAM_TX_RX_ONLY:   return "receive only";
        case HAM_TX_OFF_BAND:  return "outside the amateur bands (Settings > TX Cal)";
        case HAM_TX_OFF_RANGE: return "outside the TX ranges (Settings > TX Cal)";
        case HAM_TX_NO_RANGES: return "no TX ranges in the own list (Settings > TX Cal)";
        default:               return NULL;
    }
}

int tx_ranges_kr_count(void) {
    return KR_N;
}

void tx_ranges_kr_get(int i, int32_t *lo, int32_t *hi) {
    *lo = kr[i].lo;
    *hi = kr[i].hi;
}
