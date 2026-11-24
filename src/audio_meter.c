/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the Internet radio's station on the S-meter box (#360): its level, queue, motion and scale
 *  (audio_meter.h)
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "audio_meter.h"

#include <math.h>
#include <stdlib.h>

static float dbfs(float x) {
    return x > 0.0f ? fmaxf(20.0f * log10f(x), AUDIO_METER_FLOOR) : AUDIO_METER_FLOOR;
}

/* In integers: a piece of 2048 samples sums to under 2^42 */
audio_level_t audio_meter_measure(const int16_t *samples, size_t n) {
    audio_level_t lvl  = {AUDIO_METER_FLOOR, AUDIO_METER_FLOOR};
    int64_t       sum  = 0;
    int32_t       peak = 0;

    if (!n) {
        return lvl;
    }
    for (size_t i = 0; i < n; i++) {
        int32_t v = samples[i];

        sum += (int64_t)v * v;
        if (abs(v) > peak) {
            peak = abs(v);
        }
    }
    lvl.rms  = dbfs(sqrtf((float)((double)sum / n)) / 32768.0f);
    lvl.peak = dbfs(peak / 32768.0f);
    return lvl;
}

void audio_meter_reset(audio_meter_t *m, uint64_t now_ms) {
    m->bar     = AUDIO_METER_FLOOR;
    m->peak    = AUDIO_METER_FLOOR;
    m->peak_ms = now_ms;
    m->last_ms = now_ms;
}

void audio_meter_step(audio_meter_t *m, const audio_level_t *in, uint64_t now_ms) {
    float fall = now_ms > m->last_ms ? (now_ms - m->last_ms) * AUDIO_METER_FALL / 1000.0f : 0.0f;

    m->last_ms = now_ms;
    m->bar     = fmaxf(m->bar - fall, AUDIO_METER_FLOOR);
    if (now_ms - m->peak_ms > AUDIO_METER_HOLD_MS) {
        m->peak = fmaxf(m->peak - fall, AUDIO_METER_FLOOR);
    }
    if (in) {
        m->bar = fmaxf(m->bar, fmaxf(in->rms, AUDIO_METER_FLOOR));
        if (in->peak >= m->peak) {
            m->peak    = fminf(in->peak, 0.0f);
            m->peak_ms = now_ms;
        }
    }
    m->bar  = fminf(m->bar, 0.0f);
    m->peak = fmaxf(m->peak, m->bar);
}

float audio_meter_to_s(float dbfs) {
    float d = fminf(fmaxf(dbfs, AUDIO_METER_FLOOR), 0.0f);

    if (d <= AUDIO_METER_KNEE) {
        return AUDIO_METER_S1 +
               (d - AUDIO_METER_FLOOR) * (AUDIO_METER_S9 - AUDIO_METER_S1) / (AUDIO_METER_KNEE - AUDIO_METER_FLOOR);
    }
    return AUDIO_METER_S9 + (d - AUDIO_METER_KNEE) * (AUDIO_METER_S9_40 - AUDIO_METER_S9) / (0.0f - AUDIO_METER_KNEE);
}

/* The putting thread owns head and the entries past tail, the taking one tail: each publishes its own with a release,
 * and reads the other's with an acquire */
bool audio_meter_put(audio_meter_ring_t *r, uint64_t due_ms, audio_level_t lvl, uint32_t gen) {
    uint32_t h = atomic_load_explicit(&r->head, memory_order_relaxed);
    uint32_t t = atomic_load_explicit(&r->tail, memory_order_acquire);

    if (h - t >= AUDIO_METER_RING) {
        return false;
    }
    r->e[h % AUDIO_METER_RING].due = due_ms;
    r->e[h % AUDIO_METER_RING].lvl = lvl;
    r->e[h % AUDIO_METER_RING].gen = gen;
    atomic_store_explicit(&r->head, h + 1, memory_order_release);
    return true;
}

/* A level of an older turn goes at once, due or not: the next station's would wait behind it */
bool audio_meter_take(audio_meter_ring_t *r, uint64_t now_ms, uint32_t gen, audio_level_t *lvl) {
    while (true) {
        uint32_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);
        uint32_t h = atomic_load_explicit(&r->head, memory_order_acquire);

        if (t == h) {
            return false;
        }
        const audio_meter_entry_t *e = &r->e[t % AUDIO_METER_RING];

        if (e->gen == gen && e->due > now_ms) {
            return false;
        }
        bool mine = e->gen == gen;

        if (mine) {
            *lvl = e->lvl;
        }
        atomic_store_explicit(&r->tail, t + 1, memory_order_release);
        if (mine) {
            return true;
        }
    }
}
