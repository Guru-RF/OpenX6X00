/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - Auto Time Sync of the FT8 app: the decisions (time_sync.h)
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "time_sync.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "ft2.h"        /* ftx_slot_time, FTX_PROTOCOL_FT2 */
#include "utils.h"      /* ftx_slot_offset_ns */

#define NS 1000000000LL

bool ftx_sync_proto(int proto) {
    return proto == (int)FTX_PROTOCOL_FT8 || proto == (int)FTX_PROTOCOL_FT4;
}

static float symbol_of(int proto) {
    return proto == (int)FTX_PROTOCOL_FT8 ? FT8_SYMBOL_PERIOD : FT4_SYMBOL_PERIOD;
}

static int64_t slot_ns_of(int proto) {
    return llround(ftx_slot_time(proto) * 1e9);
}

/* FNV-1a: the call as a number, to count the stations */
static uint32_t call_hash(const char *call) {
    uint32_t h = 2166136261u;

    for (const unsigned char *p = (const unsigned char *)call; *p; p++) h = (h ^ *p) * 16777619u;
    return h;
}

/* What a right clock reads: FTX_SYNC_RX_DT_S until the radio learned its own (clock_sync.c) */
static float rx_dt = FTX_SYNC_RX_DT_S;

void ftx_sync_set_rx_dt(float dt) {
    rx_dt = dt;
}

float ftx_sync_rx_dt(void) {
    return rx_dt;
}

void ftx_sync_reset(ftx_sync_t *s) {
    s->n     = 0;
    s->proto = -1;
}

/* Another mode: what was gathered is of other slots */
static void follow_mode(ftx_sync_t *s, int proto) {
    if (s->proto != proto) {
        s->n     = 0;
        s->proto = proto;
    }
}

static bool set_since(int64_t then, int64_t now) {
    return llabs(now - then) > FTX_SYNC_SET_NS;
}

/* Where start_ns is in its slot: -slot/2 .. slot/2 from the nearest slot start */
static int64_t into_slot(int proto, int64_t start_ns) {
    return ftx_slot_offset_ns(start_ns / NS, (long)(start_ns % NS), ftx_slot_time(proto));
}

float ftx_sync_dt(int proto, int64_t start_ns, float time_sec) {
    return time_sec - FTX_SYNC_DECODER_LAG * symbol_of(proto) + (float)into_slot(proto, start_ns) / 1e9f -
           FTX_SYNC_START_S;
}

bool ftx_sync_add(ftx_sync_t *s, int proto, int64_t start_ns, int64_t start_mono_ns, float time_sec, const char *call,
                  int64_t now_ns, int64_t now_mono_ns) {
    follow_mode(s, proto);
    if (!ftx_sync_proto(proto) || !call || !call[0] || start_ns <= 0) return false;

    int64_t clock_set = start_ns - start_mono_ns;

    if (set_since(clock_set, now_ns - now_mono_ns)) return false;

    float dt = ftx_sync_dt(proto, start_ns, time_sec);

    if (!(fabsf(dt) <= FTX_SYNC_DT_MAX_S)) return false;   /* NaN too */
    if (s->n == FTX_SYNC_SAMPLES) {     /* full: the oldest goes */
        memmove(&s->s[0], &s->s[1], (FTX_SYNC_SAMPLES - 1) * sizeof(s->s[0]));
        s->n--;
    }
    s->s[s->n++] = (ftx_sync_sample_t){
        .slot      = (start_ns - into_slot(proto, start_ns)) / slot_ns_of(proto),
        .clock_set = clock_set,
        .call      = call_hash(call),
        .dt        = dt,
    };
    return true;
}

static int cmp_float(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;

    return (x > y) - (x < y);
}

static int cmp_slot(const void *a, const void *b) {
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;

    return (x > y) - (x < y);
}

/* By the sender, then by DT: each station's run in order for its median */
static int cmp_call_dt(const void *a, const void *b) {
    const ftx_sync_sample_t *x = a, *y = b;

    if (x->call != y->call) return x->call < y->call ? -1 : 1;
    return cmp_float(&x->dt, &y->dt);
}

/* The median of n sorted values (n > 0) */
static float median_sorted(const float *v, int n) {
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0f;
}

ftx_sync_rc_t ftx_sync_check(ftx_sync_t *s, int proto, int64_t now_ns, int64_t now_mono_ns, ftx_sync_result_t *r) {
    static ftx_sync_sample_t by_call[FTX_SYNC_SAMPLES];
    static int64_t           slots[FTX_SYNC_SAMPLES];
    static float             votes[FTX_SYNC_SAMPLES], dist[FTX_SYNC_SAMPLES], run[FTX_SYNC_SAMPLES];
    ftx_sync_result_t        res = {0};

    follow_mode(s, proto);
    if (r) *r = res;
    if (!ftx_sync_proto(proto)) return FTX_SYNC_WAIT;

    /* Out: the samples of a clock set since, and those older than the window (by their slot's start) */
    int64_t clock_now = now_ns - now_mono_ns, slot_ns = slot_ns_of(proto);
    int     kept      = 0;

    for (int i = 0; i < s->n; i++) {
        const ftx_sync_sample_t *x = &s->s[i];

        if (set_since(x->clock_set, clock_now) || now_ns - x->slot * slot_ns > FTX_SYNC_WINDOW_S * NS) continue;
        s->s[kept++] = *x;
    }
    s->n        = kept;
    res.decodes = kept;

    /* The slots heard */
    for (int i = 0; i < kept; i++) slots[i] = s->s[i].slot;
    qsort(slots, kept, sizeof(slots[0]), cmp_slot);
    for (int i = 0; i < kept; i++)
        if (i == 0 || slots[i] != slots[i - 1]) res.slots++;

    /* One vote a station: the median of its DTs */
    memcpy(by_call, s->s, kept * sizeof(by_call[0]));
    qsort(by_call, kept, sizeof(by_call[0]), cmp_call_dt);
    for (int i = 0; i < kept;) {
        int m = 0;

        while (i + m < kept && by_call[i + m].call == by_call[i].call) {
            run[m] = by_call[i + m].dt;
            m++;
        }
        votes[res.stations++] = median_sorted(run, m);
        i += m;
    }

    if (r) *r = res;
    if (res.stations < FTX_SYNC_MIN_STATIONS || res.slots < FTX_SYNC_MIN_SLOTS) return FTX_SYNC_WAIT;

    qsort(votes, res.stations, sizeof(votes[0]), cmp_float);
    res.median_dt = median_sorted(votes, res.stations);
    for (int i = 0; i < res.stations; i++) dist[i] = fabsf(votes[i] - res.median_dt);
    qsort(dist, res.stations, sizeof(dist[0]), cmp_float);
    res.spread = median_sorted(dist, res.stations);
    res.step   = rx_dt - res.median_dt;
    if (r) *r = res;

    if (res.spread > FTX_SYNC_SPREAD_MAX_S) return FTX_SYNC_SPREAD;
    if (fabsf(res.step) < FTX_SYNC_STEP_MIN_S) return FTX_SYNC_ON_TIME;
    if (fabsf(res.step) > FTX_SYNC_STEP_MAX_S) return FTX_SYNC_TOO_FAR;
    return FTX_SYNC_STEP;
}

bool ftx_sync_moment(int proto, float sec, float step) {
    float slot = ftx_slot_time(proto), half = slot / 2.0f, after = sec + step;

    return ftx_sync_proto(proto) && sec >= half && sec <= half + FTX_SYNC_EDGE_S && after >= FTX_SYNC_EDGE_S &&
           after <= slot - FTX_SYNC_EDGE_S;
}
