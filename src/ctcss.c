/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - a CTCSS tone under the audio the radio's Linux side transmits in FM (ctcss.h)
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "ctcss.h"

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* The 50 tones in common use (the EIA's 38 and the ones added since) */
static const int16_t tones[] = {
    670,  693,  719,  744,  770,  797,  825,  854,  885,  915,  948,  974,  1000, 1035, 1072, 1109, 1148,
    1188, 1230, 1273, 1318, 1365, 1413, 1462, 1514, 1567, 1598, 1622, 1655, 1679, 1713, 1738, 1773, 1799,
    1835, 1862, 1899, 1928, 1966, 1995, 2035, 2065, 2107, 2181, 2257, 2291, 2336, 2418, 2503, 2541,
};

#define N_TONES ((int)(sizeof(tones) / sizeof(tones[0])))

/* The two measured points (ctcss.h) */
#define REF1_TENTHS 915
#define REF1_DB (-22.5f)
#define REF2_TENTHS 1230
#define REF2_DB (-24.0f)

int ctcss_count(void) {
    return N_TONES;
}

int ctcss_tone(int i) {
    return i >= 0 && i < N_TONES ? tones[i] : CTCSS_OFF;
}

static int find(int tenths) {
    for (int i = 0; i < N_TONES; i++) {
        if (tones[i] == tenths)
            return i;
    }
    return -1;
}

int ctcss_valid(int tenths) {
    return find(tenths) >= 0 ? tenths : CTCSS_OFF;
}

int ctcss_step(int tenths, int dir) {
    int i = find(tenths);

    if (i < 0)
        return dir > 0 ? tones[0] : tones[N_TONES - 1];
    i += dir > 0 ? 1 : -1;
    return i < 0 || i >= N_TONES ? CTCSS_OFF : tones[i];
}

float ctcss_level_db(int tenths) {
    float per_octave = (REF2_DB - REF1_DB) / log2f((float)REF2_TENTHS / REF1_TENTHS);

    return REF1_DB + per_octave * log2f((float)tenths / REF1_TENTHS);
}

void ctcss_init(ctcss_t *c, int tenths, unsigned rate) {
    c->phase = 0.0;
    c->step  = 0.0;
    c->amp   = 0.0f;
    if (find(tenths) < 0 || rate == 0)
        return;
    c->step = 2.0 * M_PI * (tenths / 10.0) / rate;
    c->amp  = 32767.0f * powf(10.0f, ctcss_level_db(tenths) / 20.0f);
}

void ctcss_mix(ctcss_t *c, int16_t *buf, size_t n) {
    if (c->step == 0.0)
        return;
    for (size_t i = 0; i < n; i++) {
        float v = buf[i] + c->amp * (float)sin(c->phase);

        buf[i] = (int16_t)(v > 32767.0f ? 32767.0f : v < -32767.0f ? -32767.0f : v);
        c->phase += c->step;
        if (c->phase >= 2.0 * M_PI)
            c->phase -= 2.0 * M_PI;
    }
}
