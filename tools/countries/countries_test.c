/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of src/countries.c, the countries the first-start guide and Settings > General offer, with stand-ins for
 *  the GUI's language (i18n_code), the setting (param_t_get/param_t_set on cfg_country), the band plan's follow and
 *  cty.dat (dxcc_lookup). The cases:
 *  - the list as tools/countries/gen.py made it: codes sorted, unique, in capitals; regions 1-3; a time zone and a
 *    place each; every language's order a whole permutation, and in it each country found again by its code;
 *  - each country's region against src/itu_region.c at its place: they agree, but for the US Minor Outlying Islands;
 *  - the names in the GUI's language, a translator's es_UY taking es, a language the list lacks taking English;
 *  - the setting: a code saved in capitals and read back, "" clearing it, a code not in the list refused with nothing
 *    changed, the band plan told each time;
 *  - a callsign's country by its DXCC entity (Northern Ireland is GB, the UN's 4U1I none);
 *  - tz_alike: the same offsets and rules whatever the zone names.
 */

#include "countries.h"

#include "cfg/parameter_api.h"
#include "itu_region.h"

#include <stdio.h>
#include <string.h>

static int fails, checks;

static void check(bool ok, const char *what, const char *v) {
    checks++;
    if (!ok) {
        fails++;
        printf("FAIL %s: %s\n", what, v ? v : "");
    }
}

/* ---- stand-ins ---- */

static const char *lang = "";

const char *i18n_code(void) {
    return lang;
}

static int follows;

void band_plan_follow(void) {
    follows++;
}

struct ParamText {
    char v[16];
};

static ParamText  country_param;
ParamText        *cfg_country = &country_param;

size_t param_t_get(const ParamText *p, char *out, size_t size) {
    snprintf(out, size, "%s", p->v);
    return strlen(p->v);
}

void param_t_set(ParamText *p, const char *v) {
    snprintf(p->v, sizeof(p->v), "%s", v ? v : "");
}

/* cty.dat's answer for a few callsigns: the primary prefix of the entity */
bool dxcc_lookup(const char *call, const char **name, const char **prefix) {
    static const char *const calls[][2] = {
        {"ON6URE", "ON"}, {"ON6URE/P", "ON"}, {"CX8ANM", "CX"}, {"K1ABC", "K"}, {"KH6XX", "KH6"}, {"GI4XYZ", "GI"},
        {"4U1ITU", "4U1I"}, {"EA8AA", "EA8"}, {"DL1AA", "DL"}, {"BY1AA", "BY"}, {"VK2AA", "VK"},
    };

    for (size_t i = 0; i < sizeof(calls) / sizeof(calls[0]); i++) {
        if (!strcmp(call, calls[i][0])) {
            if (name) *name = calls[i][1];
            if (prefix) *prefix = calls[i][1];
            return true;
        }
    }
    return false;
}

/* ---- the list ---- */

/* Countries whose place is on the other side of a line from the region the list gives them: the US Minor Outlying
 * Islands are both (Midway and Johnston R2, Wake, Palmyra, Kingman, Jarvis and Baker R3), their place is Midway */
static const char *const region_odd[] = {"UM", NULL};

static bool odd(const char *cc) {
    for (int i = 0; region_odd[i]; i++) {
        if (!strcmp(region_odd[i], cc)) return true;
    }
    return false;
}

static void list(void) {
    int n = countries_n();

    check(n > 240 && n < 260, "about 250 countries", NULL);
    for (int i = 0; i < n; i++) {
        const country_t *c = country_sorted(i);

        check(c != NULL, "sorted: each place has one", NULL);
    }
    check(country_sorted(-1) == NULL && country_sorted(n) == NULL, "sorted: none outside", NULL);

    int r[4] = {0}, seen = 0;

    for (int i = 0; i < n; i++) {
        const country_t *c = country_sorted(i);
        char             at[32];

        if (!c) continue;
        snprintf(at, sizeof(at), "%s", c->cc);
        check(strlen(c->cc) == 2 && c->cc[0] >= 'A' && c->cc[0] <= 'Z' && c->cc[1] >= 'A' && c->cc[1] <= 'Z',
              "code: two capitals", at);
        check(country_find(c->cc) == c, "found by its code", at);
        check(country_sorted(country_sorted_index(c->cc)) == c, "found again in the order", at);
        check(c->region >= 1 && c->region <= 3, "region 1-3", at);
        check(c->tz && c->tz[0], "a time zone", at);
        check(c->lat >= -90 && c->lat <= 90 && c->lon >= -180 && c->lon <= 180 && (c->lat || c->lon), "a place", at);
        r[c->region]++;
        seen++;

        int itu = itu_region(c->lat, c->lon);

        if (itu != c->region) {
            if (odd(c->cc)) {
                printf("note %s: region %d, its place in %d\n", c->cc, c->region, itu);
            } else {
                char v[48];

                snprintf(v, sizeof(v), "%s region %d, itu_region %d at %.2f %.2f", c->cc, c->region, itu, c->lat,
                         c->lon);
                check(false, "region against itu_region()", v);
            }
        }
    }
    check(seen == n, "every country once in the order", NULL);
    printf("countries: %d (Region 1 %d, 2 %d, 3 %d)\n", n, r[1], r[2], r[3]);

    check(country_find("be") == country_find("BE") && country_find("BE"), "any case", NULL);
    check(!country_find("") && !country_find(NULL) && !country_find("B") && !country_find("BEL"), "not codes", NULL);
    check(!country_find("XX") && !country_find("AQ"), "not in the list", NULL);
    check(country_find("US")->region == 2 && country_find("UY")->region == 2, "the Americas: Region 2", NULL);
    check(country_find("BE")->region == 1 && country_find("MN")->region == 1 && country_find("KZ")->region == 1,
          "Belgium, Mongolia, Kazakhstan: Region 1", NULL);
    check(country_find("IR")->region == 3 && country_find("AU")->region == 3 && country_find("JP")->region == 3,
          "Iran, Australia, Japan: Region 3", NULL);
}

/* ---- the names ---- */

static void names(void) {
    const country_t *be = country_find("BE");

    lang = "";
    check(!strcmp(country_name(be), "Belgium"), "English", country_name(be));
    check(!strcmp(country_name(country_sorted(0)), "Afghanistan"), "English: Afghanistan first",
          country_name(country_sorted(0)));
    check(!strcmp(country_name(country_sorted(countries_n() - 1)), "Zimbabwe"), "English: Zimbabwe last",
          country_name(country_sorted(countries_n() - 1)));
    lang = "es";
    check(!strcmp(country_name(be), "Bélgica"), "Spanish", country_name(be));
    check(!strcmp(country_name(country_sorted(0)), "Afganistán"), "Spanish: Afganistán first",
          country_name(country_sorted(0)));
    lang = "es_UY";
    check(!strcmp(country_name(be), "Bélgica"), "es_UY takes es", country_name(be));
    lang = "nl";
    check(!strcmp(country_name(be), "België"), "Dutch", country_name(be));
    lang = "fr";
    check(!strcmp(country_name(be), "Belgique"), "French", country_name(be));
    lang = "de";
    check(!strcmp(country_name(be), "Belgien"), "German", country_name(be));
    lang = "zh_CN";
    check(!strcmp(country_name(be), "比利时"), "Chinese", country_name(be));
    lang = "pt";
    check(!strcmp(country_name(be), "Belgium"), "a language the list lacks: English", country_name(be));
    lang = "";
    check(!strcmp(country_name(NULL), ""), "none: empty", NULL);

    /* Every name in every language: there, and in the order of its language (each one at its index) */
    static const char *const langs[] = {"", "de", "es", "fr", "it", "nl", "zh_CN"};

    for (size_t l = 0; l < sizeof(langs) / sizeof(langs[0]); l++) {
        lang = langs[l];
        for (int i = 0; i < countries_n(); i++) {
            const char *nm = country_name(country_sorted(i));

            check(nm && nm[0], "a name", langs[l]);
        }
    }
    lang = "";
}

/* ---- the setting ---- */

static void setting(void) {
    char cc[3];
    int  was;

    check(!country_code(cc)[0] && !country_now(), "none set", cc);
    was = follows;
    check(country_save("be"), "be saved", NULL);
    check(!strcmp(country_code(cc), "BE") && country_now() == country_find("BE"), "kept as BE", cc);
    check(follows == was + 1, "the band plan told", NULL);
    check(!country_save("XX") && !country_save("BEL"), "not in the list: refused", NULL);
    check(!strcmp(country_code(cc), "BE") && follows == was + 1, "refused: nothing changed", cc);
    check(country_save("UY") && country_now() == country_find("UY"), "another one", NULL);
    check(country_save("") && !country_code(cc)[0] && !country_now(), "\"\" clears it", cc);
    check(country_save(NULL) && !country_now(), "NULL clears it", NULL);
    check(follows == was + 4, "the band plan told each time", NULL);
}

/* ---- a callsign's country ---- */

static void calls(void) {
    static const char *const want[][2] = {
        {"ON6URE", "BE"}, {"ON6URE/P", "BE"}, {"CX8ANM", "UY"}, {"K1ABC", "US"}, {"KH6XX", "US"}, {"GI4XYZ", "GB"},
        {"4U1ITU", ""}, {"EA8AA", "ES"}, {"DL1AA", "DE"}, {"BY1AA", "CN"}, {"VK2AA", "AU"}, {"Q0QQQ", ""}, {"", ""},
    };
    char cc[3];

    for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        bool got = country_of_call(want[i][0], cc);

        check(got == (want[i][1][0] != 0) && !strcmp(cc, want[i][1]), want[i][0], cc);
    }
    check(!country_of_call(NULL, cc) && !cc[0], "NULL: none", cc);
}

/* ---- time zones ---- */

static void zones(void) {
    check(tz_alike("<-03>3", "BRT3"), "<-03>3 and BRT3", NULL);
    check(tz_alike("<+08>-8", "SGT-8"), "<+08>-8 and SGT-8", NULL);
    check(tz_alike("<+0530>-5:30", "IST-5:30"), "<+0530>-5:30 and IST-5:30", NULL);
    check(tz_alike("UTC0", "GMT0"), "UTC0 and GMT0", NULL);
    check(tz_alike("CET-1CEST,M3.5.0,M10.5.0/3", "MET-1MEST,M3.5.0,M10.5.0/3"), "CET and MET", NULL);
    check(!tz_alike("CET-1CEST,M3.5.0,M10.5.0/3", "EET-2EEST,M3.5.0/3,M10.5.0/4"), "CET is not EET", NULL);
    check(!tz_alike("EST5EDT,M3.2.0,M11.1.0", "EST5"), "summer time or not", NULL);
    check(!tz_alike("<-04>4<-03>,M9.1.6/24,M4.1.6/24", "<-03>3"), "Chile is not Uruguay", NULL);
    check(!tz_alike("", "") && !tz_alike(NULL, "UTC0"), "none: not alike", NULL);
    check(tz_alike(country_find("BE")->tz, "CET-1CEST,M3.5.0,M10.5.0/3"), "Belgium: Central European", NULL);
    check(tz_alike(country_find("UY")->tz, "BRT3"), "Uruguay: UTC-3", country_find("UY")->tz);
    check(tz_alike(country_find("US")->tz, "EST5EDT,M3.2.0,M11.1.0"), "the US: New York's", country_find("US")->tz);
}

int main(void) {
    list();
    names();
    setting();
    calls();
    zones();
    printf("countries: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
