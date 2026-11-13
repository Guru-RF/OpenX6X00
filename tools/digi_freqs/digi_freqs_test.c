/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of src/digi_freqs.c (#205): the built-in lists of PSK, Olivia and JTTY by region, the user's own
 *  frequencies in the place of the built-in ones on their band, and the band keys' steps through the result. The band
 *  plan's region and the user's rows of params.db (cfg_digital_list) are stood in for here.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "digi_freqs.h"

#include "cfg/ham_bands.h"

#include <stdio.h>
#include <string.h>

static int fails, checks;

#define CHECK(cond, ...)                                    \
    do {                                                    \
        checks++;                                           \
        if (!(cond)) {                                      \
            fails++;                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);     \
            printf(__VA_ARGS__);                            \
            printf("\n");                                   \
        }                                                   \
    } while (0)

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* ---- What the GUI gives digi_freqs_get: the band plan's region and the rows of params.db ---------------------- */

static int                  stub_region = 1;
static cfg_digital_type_t   stub_type;
static cfg_digital_preset_t stub_rows[DIGI_FREQS_MAX * 2];
static int                  stub_n;

int band_plan_region(void) {
    return stub_region;
}

/* As the real one: the rows of that type in frequency order, at most max */
int cfg_digital_list(cfg_digital_type_t type, cfg_digital_preset_t *out, int max) {
    int k = 0;

    for (int i = 0; i < stub_n && k < max; i++) {
        if (type == stub_type) {
            out[k++] = stub_rows[i];
        }
    }
    return k;
}

static void rows(cfg_digital_type_t type, int n, const int32_t *freqs, const char *const *labels) {
    stub_type = type;
    stub_n    = n;
    for (int i = 0; i < n; i++) {
        stub_rows[i].freq = freqs[i];
        snprintf(stub_rows[i].label, sizeof(stub_rows[i].label), "%s", labels ? labels[i] : "");
    }
}

/* ---- Helpers -------------------------------------------------------------------------------------------------- */

static digi_freq_t own(int32_t freq, const char *label) {
    digi_freq_t e = {.freq = freq};

    snprintf(e.label, sizeof(e.label), "%s", label);
    return e;
}

/* The entry of list at freq, NULL when there is none */
static const digi_freq_t *at(const digi_freq_t *list, int n, int32_t freq) {
    for (int i = 0; i < n; i++) {
        if (list[i].freq == freq) {
            return &list[i];
        }
    }
    return NULL;
}

/* The entries of list on the amateur band of freq */
static int on_band(const digi_freq_t *list, int n, int32_t freq) {
    int32_t lo, hi;
    int     k = 0;

    if (!ham_band_of(freq, &lo, &hi)) {
        return 0;
    }
    for (int i = 0; i < n; i++) {
        k += list[i].freq >= lo && list[i].freq <= hi;
    }
    return k;
}

static bool ordered(const digi_freq_t *list, int n) {
    for (int i = 1; i < n; i++) {
        if (list[i].freq <= list[i - 1].freq) {
            return false;
        }
    }
    return true;
}

/* The same entries: frequency, label and own (the bytes after a label's end are not the list's) */
static bool same_list(const digi_freq_t *a, int na, const digi_freq_t *b, int nb) {
    if (na != nb) {
        return false;
    }
    for (int i = 0; i < na; i++) {
        if (a[i].freq != b[i].freq || strcmp(a[i].label, b[i].label) || a[i].own != b[i].own) {
            return false;
        }
    }
    return true;
}

static const cfg_digital_type_t types[] = {CFG_DIG_TYPE_PSK, CFG_DIG_TYPE_OLIVIA, CFG_DIG_TYPE_JTTY};
static const char *const        type_names[] = {"PSK", "Olivia", "JTTY"};

/* ---- The built-in lists --------------------------------------------------------------------------------------- */

static void builtin_cases(void) {
    digi_freq_t l[DIGI_FREQS_MAX], r1[DIGI_FREQS_MAX];
    int         n, n1;

    /* Every list, every region: in frequency order, every entry on an amateur band and not an own one, each band
     * label once, the same bands in every region */
    for (int t = 0; t < COUNT(types); t++) {
        n1 = digi_freqs_builtin(types[t], 1, r1, DIGI_FREQS_MAX);
        CHECK(n1 >= 9, "%s: Region 1 has %d entries", type_names[t], n1);
        for (int region = 1; region <= 3; region++) {
            n = digi_freqs_builtin(types[t], region, l, DIGI_FREQS_MAX);
            CHECK(n == n1, "%s: Region %d has %d entries, Region 1 %d", type_names[t], region, n, n1);
            CHECK(ordered(l, n), "%s: Region %d in frequency order", type_names[t], region);
            for (int i = 0; i < n; i++) {
                CHECK(ham_band_of(l[i].freq, NULL, NULL), "%s: Region %d %d Hz on an amateur band", type_names[t],
                      region, (int)l[i].freq);
                CHECK(ham_band_of(l[i].freq + 3000, NULL, NULL), "%s: Region %d %d Hz: the passband on the band too",
                      type_names[t], region, (int)l[i].freq);
                CHECK(!l[i].own, "%s: Region %d %s not the user's", type_names[t], region, l[i].label);
                CHECK(l[i].label[0], "%s: Region %d %d Hz has a label", type_names[t], region, (int)l[i].freq);
                for (int j = 0; j < i; j++) {
                    CHECK(strcmp(l[i].label, l[j].label), "%s: Region %d label %s twice", type_names[t], region,
                          l[i].label);
                }
                CHECK(i < n1 && !strcmp(l[i].label, r1[i].label), "%s: Region %d entry %d is %s, Region 1's %s",
                      type_names[t], region, i, l[i].label, i < n1 ? r1[i].label : "none");
            }
        }
    }

    /* PSK on 40 m (#205): Region 1 7.040, Region 2 and the US 7.070, Region 3 7.035; the rest the same */
    n = digi_freqs_builtin(CFG_DIG_TYPE_PSK, 1, l, DIGI_FREQS_MAX);
    CHECK(at(l, n, 7040000) && !at(l, n, 7070000) && !at(l, n, 7035000), "PSK Region 1: 7.040");
    CHECK(at(l, n, 14070000) && at(l, n, 3580000) && at(l, n, 18097000), "PSK Region 1: 14.070, 3.580, 18.097");
    n = digi_freqs_builtin(CFG_DIG_TYPE_PSK, 2, l, DIGI_FREQS_MAX);
    CHECK(at(l, n, 7070000) && !at(l, n, 7040000) && !at(l, n, 7035000), "PSK Region 2: 7.070");
    CHECK(at(l, n, 7070000) && !strcmp(at(l, n, 7070000)->label, "40m"), "PSK Region 2: 7.070 is 40m");
    CHECK(at(l, n, 14070000) && at(l, n, 3580000), "PSK Region 2: 14.070, 3.580");
    n = digi_freqs_builtin(CFG_DIG_TYPE_PSK, 3, l, DIGI_FREQS_MAX);
    CHECK(at(l, n, 7035000) && !at(l, n, 7040000) && !at(l, n, 7070000), "PSK Region 3: 7.035");

    /* Anything but 2 and 3 is Region 1, as band_plan_region() never gives it */
    for (int region = -1; region <= 5; region += 1) {
        if (region == 2 || region == 3) {
            continue;
        }
        n = digi_freqs_builtin(CFG_DIG_TYPE_PSK, region, l, DIGI_FREQS_MAX);
        CHECK(at(l, n, 7040000) && !at(l, n, 7070000), "PSK region %d taken as Region 1", region);
    }

    /* Olivia: Region 2's 160 m 1825.5 and 40 m 7038.5 (oliviadigitalmode.org); Regions 1 and 3 1837.5 and 7071.0 */
    n = digi_freqs_builtin(CFG_DIG_TYPE_OLIVIA, 2, l, DIGI_FREQS_MAX);
    CHECK(at(l, n, 1825500) && at(l, n, 7038500) && !at(l, n, 1837500) && !at(l, n, 7071000), "Olivia Region 2");
    CHECK(at(l, n, 14071000) && at(l, n, 14107000), "Olivia Region 2: 20m and 20m wide");
    for (int region = 1; region <= 3; region += 2) {
        n = digi_freqs_builtin(CFG_DIG_TYPE_OLIVIA, region, l, DIGI_FREQS_MAX);
        CHECK(at(l, n, 1837500) && at(l, n, 7071000) && !at(l, n, 1825500) && !at(l, n, 7038500),
              "Olivia Region %d", region);
    }

    /* JTTY: WSJT-X's one list, 14.090 on 20 m */
    n = digi_freqs_builtin(CFG_DIG_TYPE_JTTY, 2, l, DIGI_FREQS_MAX);
    CHECK(n == 9 && at(l, n, 14090000) && at(l, n, 7090000), "JTTY: 9 bands, 14.090 and 7.090");

    /* The other types have no built-in list: their rows are their whole list (cfg_digital_load) */
    CHECK(digi_freqs_builtin(CFG_DIG_TYPE_FT8, 1, l, DIGI_FREQS_MAX) == 0, "FT8: no built-in list");
    CHECK(digi_freqs_builtin(CFG_DIG_TYPE_WSPR, 1, l, DIGI_FREQS_MAX) == 0, "WSPR: no built-in list");
    CHECK(digi_freqs_builtin((cfg_digital_type_t)99, 1, l, DIGI_FREQS_MAX) == 0, "type 99: none");

    /* max respected, the lowest first */
    n = digi_freqs_builtin(CFG_DIG_TYPE_PSK, 1, l, 3);
    CHECK(n == 3 && l[0].freq == 1838000 && l[2].freq == 7040000, "PSK max 3: 160m, 80m, 40m");
    CHECK(digi_freqs_builtin(CFG_DIG_TYPE_PSK, 1, l, 0) == 0, "max 0: nothing");
}

/* ---- The user's own in the built-in list ---------------------------------------------------------------------- */

static void merge_cases(void) {
    digi_freq_t l[DIGI_FREQS_MAX], base[DIGI_FREQS_MAX], o[DIGI_FREQS_MAX + 4];
    int         n, nb;

    /* None: the built-in list */
    nb = digi_freqs_builtin(CFG_DIG_TYPE_PSK, 1, base, DIGI_FREQS_MAX);
    n  = digi_freqs_merge(CFG_DIG_TYPE_PSK, 1, NULL, 0, l, DIGI_FREQS_MAX);
    CHECK(same_list(l, n, base, nb), "no own entry: the built-in list");

    /* #205: US PSK on 40 m in a Region 1 band plan: 7.070 in the place of 7.040, its label the band's */
    o[0] = own(7070000, "");
    n    = digi_freqs_merge(CFG_DIG_TYPE_PSK, 1, o, 1, l, DIGI_FREQS_MAX);
    CHECK(n == nb, "one own entry on 40 m: as many entries (%d, %d)", n, nb);
    CHECK(!at(l, n, 7040000), "7.040 replaced");
    CHECK(at(l, n, 7070000) && at(l, n, 7070000)->own, "7.070, the user's");
    CHECK(at(l, n, 7070000) && !strcmp(at(l, n, 7070000)->label, "40m"), "no label: the band's, 40m");
    CHECK(ordered(l, n), "in frequency order");
    CHECK(at(l, n, 3580000) && !at(l, n, 3580000)->own && at(l, n, 10140000), "the other bands as they were");

    /* Its own label kept */
    o[0] = own(7070000, "40m US");
    n    = digi_freqs_merge(CFG_DIG_TYPE_PSK, 1, o, 1, l, DIGI_FREQS_MAX);
    CHECK(at(l, n, 7070000) && !strcmp(at(l, n, 7070000)->label, "40m US"), "a label of its own kept");

    /* Two on 40 m: both, the built-in one gone */
    o[0] = own(7035000, "");
    o[1] = own(7070000, "");
    n    = digi_freqs_merge(CFG_DIG_TYPE_PSK, 1, o, 2, l, DIGI_FREQS_MAX);
    CHECK(n == nb + 1 && on_band(l, n, 7000000) == 2 && !at(l, n, 7040000), "two own on 40 m: both, 7.040 gone");
    CHECK(ordered(l, n), "two own: in frequency order");

    /* The same as the built-in one: there once, the user's */
    o[0] = own(7040000, "");
    n    = digi_freqs_merge(CFG_DIG_TYPE_PSK, 1, o, 1, l, DIGI_FREQS_MAX);
    CHECK(n == nb && at(l, n, 7040000) && at(l, n, 7040000)->own, "own on the built-in frequency: once, own");

    /* On a band without a built-in one (60 m): added, nothing replaced, no band label to take */
    o[0] = own(5366500, "");
    n    = digi_freqs_merge(CFG_DIG_TYPE_PSK, 1, o, 1, l, DIGI_FREQS_MAX);
    CHECK(n == nb + 1 && at(l, n, 5366500) && at(l, n, 7040000) && at(l, n, 3580000), "60 m: added");
    CHECK(at(l, n, 5366500) && !at(l, n, 5366500)->label[0], "60 m, no label: none");
    CHECK(ordered(l, n), "60 m: in frequency order");

    /* On no amateur band (CB): added, nothing replaced */
    o[0] = own(27555000, "CB");
    n    = digi_freqs_merge(CFG_DIG_TYPE_PSK, 1, o, 1, l, DIGI_FREQS_MAX);
    CHECK(n == nb + 1 && at(l, n, 27555000) && at(l, n, 28120000) && at(l, n, 24920000), "off the bands: added");

    /* 0 Hz and less: left out, replacing nothing */
    o[0] = own(0, "zero");
    o[1] = own(-7070000, "negative");
    n    = digi_freqs_merge(CFG_DIG_TYPE_PSK, 1, o, 2, l, DIGI_FREQS_MAX);
    CHECK(same_list(l, n, base, nb), "0 Hz and less: the built-in list");

    /* The same frequency twice (not possible in the table, UNIQUE): once */
    o[0] = own(7070000, "a");
    o[1] = own(7070000, "b");
    n    = digi_freqs_merge(CFG_DIG_TYPE_PSK, 1, o, 2, l, DIGI_FREQS_MAX);
    CHECK(n == nb && at(l, n, 7070000) && !strcmp(at(l, n, 7070000)->label, "a"), "twice: once, the first");

    /* Olivia's 20 m has two built-in entries: one of the user's there takes the place of both */
    nb   = digi_freqs_builtin(CFG_DIG_TYPE_OLIVIA, 1, base, DIGI_FREQS_MAX);
    o[0] = own(14072000, "");
    n    = digi_freqs_merge(CFG_DIG_TYPE_OLIVIA, 1, o, 1, l, DIGI_FREQS_MAX);
    CHECK(n == nb - 1 && on_band(l, n, 14000000) == 1 && !at(l, n, 14107000), "Olivia 20 m: one for both");
    CHECK(at(l, n, 14072000) && !strcmp(at(l, n, 14072000)->label, "20m"), "Olivia 20 m: the label 20m");

    /* A label that fills its field without an end: cut, ended */
    o[0] = own(7070000, "");
    memset(o[0].label, 'x', sizeof(o[0].label));
    n = digi_freqs_merge(CFG_DIG_TYPE_PSK, 1, o, 1, l, DIGI_FREQS_MAX);
    CHECK(at(l, n, 7070000) && strlen(at(l, n, 7070000)->label) == sizeof(o[0].label) - 1, "an unended label ended");

    /* More than fits: DIGI_FREQS_MAX, the lowest, no write past out */
    for (int i = 0; i < DIGI_FREQS_MAX + 4; i++) {
        o[i] = own(27000000 + 10000 * i, "");
    }
    {
        digi_freq_t big[DIGI_FREQS_MAX + 1];

        big[DIGI_FREQS_MAX].freq = 12345;
        n = digi_freqs_merge(CFG_DIG_TYPE_PSK, 1, o, DIGI_FREQS_MAX + 4, big, DIGI_FREQS_MAX);
        CHECK(n == DIGI_FREQS_MAX && big[DIGI_FREQS_MAX].freq == 12345, "full: %d entries, nothing past", n);
        CHECK(ordered(big, n) && big[0].freq == 1838000, "full: in order, from 160 m");
        CHECK(big[n - 1].freq < 27000000 + 10000 * (DIGI_FREQS_MAX + 3), "full: the highest went");
    }
    n = digi_freqs_merge(CFG_DIG_TYPE_PSK, 1, o, 1, l, 2);
    CHECK(n == 2 && l[0].freq == 1838000 && l[1].freq == 3580000, "max 2: the lowest two");
    CHECK(digi_freqs_merge(CFG_DIG_TYPE_PSK, 1, o, 1, l, 0) == 0, "max 0: nothing");

    /* A type without a built-in list: the user's alone */
    o[0] = own(14074000, "FT8 20m");
    n    = digi_freqs_merge(CFG_DIG_TYPE_FT8, 1, o, 1, l, DIGI_FREQS_MAX);
    CHECK(n == 1 && l[0].freq == 14074000 && l[0].own, "FT8: the row alone");
}

/* ---- What the apps read: the region and the rows of the GUI --------------------------------------------------- */

static void get_cases(void) {
    digi_freq_t l[DIGI_FREQS_MAX];
    int         n;

    /* The band plan's region counts */
    rows(CFG_DIG_TYPE_PSK, 0, NULL, NULL);
    stub_region = 2;
    n           = digi_freqs_get(CFG_DIG_TYPE_PSK, l, DIGI_FREQS_MAX);
    CHECK(at(l, n, 7070000) && !at(l, n, 7070000)->own && !at(l, n, 7040000), "get, Region 2: 7.070 built in");
    stub_region = 1;
    n           = digi_freqs_get(CFG_DIG_TYPE_PSK, l, DIGI_FREQS_MAX);
    CHECK(at(l, n, 7040000) && !at(l, n, 7070000), "get, Region 1: 7.040");

    /* A row of PSK: in; the rows of another type are not */
    {
        static const int32_t     f[]   = {7035000, 14072000};
        static const char *const lab[] = {"PSK 40m", "PSK 20m"};

        rows(CFG_DIG_TYPE_PSK, 2, f, lab);
        n = digi_freqs_get(CFG_DIG_TYPE_PSK, l, DIGI_FREQS_MAX);
        CHECK(at(l, n, 7035000) && at(l, n, 7035000)->own && !strcmp(at(l, n, 7035000)->label, "PSK 40m"),
              "get: the row's frequency and label");
        CHECK(!at(l, n, 7040000) && !at(l, n, 14070000) && at(l, n, 14072000), "get: in the place of 7.040, 14.070");
        n = digi_freqs_get(CFG_DIG_TYPE_OLIVIA, l, DIGI_FREQS_MAX);
        CHECK(!at(l, n, 7035000) && at(l, n, 7071000), "get: PSK's rows not in Olivia's list");
    }
    rows(CFG_DIG_TYPE_PSK, 0, NULL, NULL);
}

/* ---- The band keys -------------------------------------------------------------------------------------------- */

static void step_cases(void) {
    digi_freq_t l[DIGI_FREQS_MAX];
    int         n = digi_freqs_builtin(CFG_DIG_TYPE_PSK, 1, l, DIGI_FREQS_MAX);
    digi_freq_t o[1];

    /* Opening the window: the closest, the lower of two as close */
    CHECK(digi_freqs_step(l, n, 7074000, 0) == 2, "7.074: 40m");
    CHECK(digi_freqs_step(l, n, 7070000, 0) == 2, "7.070 (km4gkd's dial): 40m");
    CHECK(digi_freqs_step(l, n, 0, 0) == 0, "0 Hz: 160m");
    CHECK(digi_freqs_step(l, n, 145000000, 0) == n - 1, "2 m: 6m");
    CHECK(digi_freqs_step(l, n, (3580000 + 7040000) / 2, 0) == 1, "half way 80m-40m: 80m");

    /* The band keys: the next above or below, round at the ends; from an entry, never that entry */
    CHECK(digi_freqs_step(l, n, 7040000, 1) == 3, "up from 7.040: 30m");
    CHECK(digi_freqs_step(l, n, 7040000, -1) == 1, "down from 7.040: 80m");
    CHECK(digi_freqs_step(l, n, l[n - 1].freq, 1) == 0, "up from 6m: 160m");
    CHECK(digi_freqs_step(l, n, l[0].freq, -1) == n - 1, "down from 160m: 6m");
    CHECK(digi_freqs_step(l, n, 7050000, 1) == 3 && digi_freqs_step(l, n, 7050000, -1) == 2, "between: either side");

    /* Stepping on from an entry the web page took away meanwhile: from its frequency */
    o[0] = own(7070000, "");
    n    = digi_freqs_merge(CFG_DIG_TYPE_PSK, 1, o, 1, l, DIGI_FREQS_MAX);
    CHECK(l[digi_freqs_step(l, n, 7040000, 1)].freq == 7070000, "up from 7.040 gone: 7.070 above it");
    CHECK(l[digi_freqs_step(l, n, 7040000, -1)].freq == 3580000, "down from 7.040 gone: 3.580");

    CHECK(digi_freqs_step(l, 0, 7040000, 0) == -1, "empty: -1");
    CHECK(digi_freqs_step(l, 0, 7040000, 1) == -1 && digi_freqs_step(l, 0, 7040000, -1) == -1, "empty, keys: -1");
    CHECK(digi_freqs_step(l, 1, 7040000, 1) == 0 && digi_freqs_step(l, 1, 7040000, -1) == 0, "one entry: itself");
}

int main(void) {
    builtin_cases();
    merge_cases();
    get_cases();
    step_cases();
    printf("digi_freqs: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
