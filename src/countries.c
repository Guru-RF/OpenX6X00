/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the countries and the one the radio is used in (countries.h)
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "countries.h"

#include "band_plan.h"
#include "cfg/cfg_api.h"
#include "dxcc.h"
#include "flags.h"
#include "i18n.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "countries_data.inc"

/* The column of the GUI's language in country_names: its file's code, or its language without the country (a
 * translator's es_UY takes es); English (0) when the list has none */
static int lang_col(void) {
    const char *code = i18n_code();
    size_t      n;

    if (!code || !code[0]) {
        return 0;
    }
    for (int i = 1; i < COUNTRY_LANGS; i++) {
        if (!strcmp(country_langs[i], code)) {
            return i;
        }
    }
    n = strcspn(code, "_");
    for (int i = 1; i < COUNTRY_LANGS; i++) {
        if (strlen(country_langs[i]) == n && !strncmp(country_langs[i], code, n)) {
            return i;
        }
    }
    return 0;
}

int countries_n(void) {
    return COUNTRIES_N;
}

const country_t *country_find(const char *cc) {
    if (!cc || strlen(cc) != 2) {
        return NULL;
    }
    for (int i = 0; i < COUNTRIES_N; i++) {
        if (!strcasecmp(country_tab[i].cc, cc)) {
            return &country_tab[i];
        }
    }
    return NULL;
}

const char *country_name(const country_t *c) {
    return c ? country_names[c - country_tab][lang_col()] : "";
}

const country_t *country_sorted(int i) {
    return i >= 0 && i < COUNTRIES_N ? &country_tab[country_order[lang_col()][i]] : NULL;
}

int country_sorted_index(const char *cc) {
    const country_t *c = country_find(cc);
    const uint16_t  *order = country_order[lang_col()];

    for (int i = 0; c && i < COUNTRIES_N; i++) {
        if (order[i] == c - country_tab) {
            return i;
        }
    }
    return -1;
}

/* ---- the setting ---- */

const char *country_code(char *cc) {
    cc[0] = 0;
    if (cfg_country) {
        param_t_get(cfg_country, cc, 3);
    }
    return cc;
}

const country_t *country_now(void) {
    char cc[3];

    return country_find(country_code(cc));
}

bool country_save(const char *cc) {
    const country_t *c = country_find(cc);

    if (cc && cc[0] && !c) {
        return false;
    }
    param_t_set(cfg_country, c ? c->cc : "");
    band_plan_follow();     /* Auto without a GPS fix or a locator: the country's region */
    return true;
}

/* ---- from a callsign ---- */

bool country_of_call(const char *call, char *cc) {
    const char *prefix;

    cc[0] = 0;
    if (!call || !call[0] || !dxcc_lookup(call, NULL, &prefix)) {
        return false;
    }
    for (size_t i = 0; i < sizeof(flag_of) / sizeof(flag_of[0]); i++) {
        if (!strcmp(flag_of[i].prefix, prefix)) {
            const char *iso = flag_iso[flag_of[i].flag];
            char        up[3] = {(char)toupper((unsigned char)iso[0]), (char)toupper((unsigned char)iso[1]), 0};

            if (!strcmp(up, "XI")) {    /* the flags' own code for Northern Ireland */
                strcpy(up, "GB");
            }
            if (!country_find(up)) {    /* 4U1I and 4U1V (the UN's), Kosovo */
                return false;
            }
            memcpy(cc, up, 3);
            return true;
        }
    }
    return false;
}

/* ---- time zones ---- */

/* A POSIX TZ string without its names: the offsets and the rules as they are, the summer time's name as '|' (it
 * has one or not), a '+' left out */
static void tz_bare(const char *s, char *out, size_t size) {
    size_t n     = 0;
    int    names = 0;

    while (*s && *s != ',' && n + 2 < size) {
        if (*s == '<' || isalpha((unsigned char)*s)) {
            if (*s == '<') {
                const char *e = strchr(s, '>');

                s = e ? e + 1 : s + strlen(s);
            } else {
                while (isalpha((unsigned char)*s)) {
                    s++;
                }
            }
            if (names++) {
                out[n++] = '|';
            }
        } else if (*s == '+') {
            s++;
        } else {
            out[n++] = *s++;
        }
    }
    snprintf(out + n, size - n, "%s", s);   /* the rules, after the first comma */
}

bool tz_alike(const char *a, const char *b) {
    char x[64], y[64];

    if (!a || !b || !a[0] || !b[0]) {
        return false;
    }
    tz_bare(a, x, sizeof(x));
    tz_bare(b, y, sizeof(y));
    return !strcmp(x, y);
}
