/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  The keyboard modes' dial frequencies (digi_freqs.h, #205).
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "digi_freqs.h"

#include "band_plan.h"
#include "cfg/ham_bands.h"

#include <stdio.h>
#include <string.h>

/* The regions an entry is for, as bits */
#define R1  1
#define R2  2
#define R3  4
#define ALL (R1 | R2 | R3)

typedef struct {
    const char *label;
    int32_t     freq;
    uint8_t     regions;
} builtin_t;

/*
 * PSK31 and PSK63 (USB). PSK31 has no single official list of frequencies. These are where activity is found,
 * cross-checked between several published lists; the signals sit within the 3 kHz above the dial, which is exactly
 * what the waterfall shows.
 *
 *  - 40 m: 7040 kHz in Region 1, where the digimodes start at 7040 since the 2009 band plan; 7070 in Region 2 and
 *    the US, the RTTY/data part of the US band; 7035 in Region 3, inside the 7030-7040 data part of the plans there
 *    (7040 and up is phone in Australia).
 *  - 17 m: 18097 kHz. PSK31 moved down from 18100 in 2019, when FT8 took that frequency.
 *  - 60 m is left out: allocations and permitted modes differ from country to country and there is no common PSK31
 *    frequency.
 */
static const builtin_t psk[] = {
    {"160m", 1838000, ALL},
    {"80m", 3580000, ALL},
    {"40m", 7035000, R3},
    {"40m", 7040000, R1},
    {"40m", 7070000, R2},
    {"30m", 10140000, ALL},
    {"20m", 14070000, ALL},
    {"17m", 18097000, ALL},
    {"15m", 21070000, ALL},
    {"12m", 24920000, ALL},
    {"10m", 28120000, ALL},
    {"6m", 50290000, ALL},
};

/*
 * Olivia (USB), from the calling frequency table of oliviadigitalmode.org: the dial for 8/250 with the waterfall
 * centre at 1500 Hz, which is where opening the window and every band change put the finder. The table has two
 * entries for Region 2: 160 m 1825.5 (Region 1 1837.5) and 40 m 7038.5, the 7.040 MHz RTTY/data frequency of the US
 * band plan (Region 1 the secondary 7071.0, as 7038.5 puts the signal on the edge of the Region 1 CW part). It names
 * none for Region 3, which keeps the Region 1 ones, inside its data parts. 20 m has a second, 32/1000 channel. No 6 m
 * channel is listed.
 */
static const builtin_t olivia[] = {
    {"160m", 1825500, R2},
    {"160m", 1837500, R1 | R3},
    {"80m", 3581500, ALL},
    {"40m", 7038500, R2},
    {"40m", 7071000, R1 | R3},
    {"30m", 10141500, ALL},
    {"20m", 14071000, ALL},
    {"20m wide", 14107000, ALL},
    {"17m", 18097500, ALL},
    {"15m", 21071000, ALL},
    {"12m", 24921000, ALL},
    {"10m", 28121000, ALL},
};

/* JTTY dial frequencies of WSJT-X 3.2 (models/FrequencyList.cpp; 14.090 MHz is the suggested meeting place), USB, the
 * signals 200-2800 Hz above; one list for every region */
static const builtin_t jtty[] = {
    {"160m", 1838000, ALL},
    {"80m", 3575000, ALL},
    {"40m", 7090000, ALL},
    {"30m", 10140000, ALL},
    {"20m", 14090000, ALL},
    {"17m", 18100000, ALL},
    {"15m", 21090000, ALL},
    {"12m", 24920000, ALL},
    {"10m", 28090000, ALL},
};

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

int digi_freqs_builtin(cfg_digital_type_t type, int region, digi_freq_t *out, int max) {
    const builtin_t *list;
    int              n, k = 0;
    uint8_t          bit = region == 2 ? R2 : region == 3 ? R3 : R1;

    switch (type) {
        case CFG_DIG_TYPE_PSK:    list = psk;    n = COUNT(psk);    break;
        case CFG_DIG_TYPE_OLIVIA: list = olivia; n = COUNT(olivia); break;
        case CFG_DIG_TYPE_JTTY:   list = jtty;   n = COUNT(jtty);   break;
        default:                  return 0;
    }
    for (int i = 0; i < n && k < max; i++) {
        if (list[i].regions & bit) {
            out[k].freq = list[i].freq;
            snprintf(out[k].label, sizeof(out[k].label), "%s", list[i].label);
            out[k].own = false;
            k++;
        }
    }
    return k;
}

/* e into list (k entries, in frequency order) at its place; past max the highest goes. A frequency already there
 * stays as it is */
static void insert(digi_freq_t *list, int *k, int max, const digi_freq_t *e) {
    int at = 0;

    while (at < *k && list[at].freq < e->freq) {
        at++;
    }
    if (at >= max || (at < *k && list[at].freq == e->freq)) {
        return;
    }
    if (*k == max) {
        (*k)--;
    }
    memmove(&list[at + 1], &list[at], (size_t)(*k - at) * sizeof(*list));
    list[at] = *e;
    (*k)++;
}

/* The amateur band of freq is the one of the built-in entry b */
static bool same_band(int32_t freq, int32_t b) {
    int32_t lo, hi;

    return ham_band_of(freq, &lo, &hi) && b >= lo && b <= hi;
}

int digi_freqs_merge(cfg_digital_type_t type, int region, const digi_freq_t *own, int n, digi_freq_t *out, int max) {
    digi_freq_t base[DIGI_FREQS_MAX];
    int         nb = digi_freqs_builtin(type, region, base, DIGI_FREQS_MAX);
    int         k  = 0;

    if (max <= 0) {
        return 0;
    }
    for (int i = 0; i < nb; i++) {
        bool replaced = false;

        for (int j = 0; j < n && !replaced; j++) {
            replaced = own[j].freq > 0 && same_band(own[j].freq, base[i].freq);
        }
        if (!replaced) {
            insert(out, &k, max, &base[i]);
        }
    }
    for (int j = 0; j < n; j++) {
        digi_freq_t e = own[j];

        if (e.freq <= 0) {
            continue;
        }
        e.label[sizeof(e.label) - 1] = '\0';
        e.own = true;
        /* No label of its own: the band's, as the built-in one it replaced had it */
        for (int i = 0; i < nb && !e.label[0]; i++) {
            if (same_band(e.freq, base[i].freq)) {
                snprintf(e.label, sizeof(e.label), "%s", base[i].label);
            }
        }
        insert(out, &k, max, &e);
    }
    return k;
}

int digi_freqs_get(cfg_digital_type_t type, digi_freq_t *out, int max) {
    cfg_digital_preset_t rows[DIGI_FREQS_MAX];
    digi_freq_t          own[DIGI_FREQS_MAX];
    int                  n = cfg_digital_list(type, rows, DIGI_FREQS_MAX);

    for (int i = 0; i < n; i++) {
        own[i].freq = rows[i].freq;
        snprintf(own[i].label, sizeof(own[i].label), "%s", rows[i].label);
        own[i].own = true;
    }
    return digi_freqs_merge(type, band_plan_region(), own, n, out, max);
}

int digi_freqs_step(const digi_freq_t *list, int n, int32_t freq, int dir) {
    int     best = -1;
    int64_t best_d = 0;

    if (n <= 0) {
        return -1;
    }
    if (dir > 0) {
        for (int i = 0; i < n; i++) {
            if (list[i].freq > freq) {
                return i;
            }
        }
        return 0;
    }
    if (dir < 0) {
        for (int i = n - 1; i >= 0; i--) {
            if (list[i].freq < freq) {
                return i;
            }
        }
        return n - 1;
    }
    for (int i = 0; i < n; i++) {
        int64_t d = (int64_t)list[i].freq - freq;

        if (d < 0) {
            d = -d;
        }
        if (best < 0 || d < best_d) {
            best   = i;
            best_d = d;
        }
    }
    return best;
}
