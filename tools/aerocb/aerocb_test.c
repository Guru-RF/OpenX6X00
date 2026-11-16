/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of src/aero.c, the table of the HF Air Band app: every frequency of the ITU Plan's families on a channel
 *  of its raster (Appendix 27, No. 27/18), ascending, the places the CX8ANM list had wrong, the areas. No frequency
 *  of it is in an amateur band, so main's rule decides PTT there as anywhere outside them: refused with TX outside ham
 *  bands off, allowed with it on (tx_ranges_check; aerocb_tx_test.cpp does the same through the real cfg_tx_refusal).
 */

#include "aero.h"
#include "cfg/tx_ranges.h"

#include <stdio.h>
#include <string.h>

static int fails, checks;

static void check(bool ok, const char *what, long v) {
    checks++;
    if (!ok) {
        fails++;
        printf("FAIL %s: %ld\n", what, v);
    }
}

/* Outside the amateur bands, where main's rule goes by TX outside ham bands: off refused, on allowed */
static bool off_band(int32_t hz) {
    return tx_ranges_check(TX_RANGES_BANDS, hz, true, 0, false) == HAM_TX_OFF_BAND &&
           tx_ranges_check(TX_RANGES_BANDS, hz, true, 0, true) == HAM_TX_OK;
}

/* The aeronautical (R) channels of Appendix 27, No. 27/18: each band's first and last carrier, 3 kHz apart */
static const struct {
    int first, last;
} raster[] = {
    {2851, 3019},   {3401, 3497},   {4651, 4696},   {5451, 5475},   {5481, 5676},   {6526, 6682},
    {8816, 8960},   {10006, 10096}, {11276, 11396}, {13261, 13357}, {17901, 17967}, {21925, 21997},
};

static bool on_raster(int khz) {
    if (khz == 3023 || khz == 5680) return true;    /* the common ones, Article 3 */
    for (size_t i = 0; i < sizeof(raster) / sizeof(raster[0]); i++)
        if (khz >= raster[i].first && khz <= raster[i].last && (khz - raster[i].first) % 3 == 0) return true;
    return false;
}

static const aero_family_t *find(const char *code, const char *name) {
    for (int i = 0; i < aero_families(); i++) {
        const aero_family_t *f = aero_family(i);

        if (!strcmp(f->code, code) && (!name || !strcmp(f->name, name))) return f;
    }
    return NULL;
}

static bool has(const aero_family_t *f, int khz) {
    for (const uint16_t *k = f ? f->khz : NULL; k && *k; k++)
        if (*k == khz) return true;
    return false;
}

static int size(const aero_family_t *f) {
    int n = 0;

    for (const uint16_t *k = f->khz; *k; k++) n++;
    return n;
}

static void test_aero_lists(void) {
    int total = 0;

    for (int i = 0; i < aero_families(); i++) {
        const aero_family_t *f    = aero_family(i);
        bool                 plan = strcmp(f->code, "VOLMET") != 0;  /* the EiBi stations: outside the Plan's lists */

        check(f->code[0] && f->name[0], "family named", i);
        check(f->khz[0] != 0, "family has frequencies", i);
        for (const uint16_t *k = f->khz; *k; k++) {
            check(k == f->khz || *k > k[-1], "ascending", *k);
            check(*k >= 2850 && *k <= 22000, "in the HF aero bands", *k);
            if (plan) check(on_raster(*k), "on a channel of the Plan", *k);
            check(off_band(*k * 1000), "aero frequency in an amateur band", *k);
            total++;
        }
        check(f->kind != AERO_LDOC ? f->areas != 0 : f->areas == 0, "areas", i);
    }
    check(aero_family(-1) == NULL && aero_family(aero_families()) == NULL, "no family", 0);

    /* The 15 routes and 9 VOLMET areas of the Plan */
    static const char *const mwara[] = {"AFI", "CAR", "CEP", "CWP", "EA", "EUR", "INO", "MID",
                                        "NAT", "NCA", "NP", "SAM", "SAT", "SEA", "SP"};
    for (size_t i = 0; i < sizeof(mwara) / sizeof(mwara[0]); i++) {
        const aero_family_t *f = find(mwara[i], NULL);

        check(f && f->kind == AERO_MWARA, "MWARA there", (long)i);
    }
    static const char *const volmet[] = {"AFI", "CAR", "EUR", "MID", "NAT", "NCA", "PAC", "SAM", "SEA"};
    for (size_t i = 0; i < sizeof(volmet) / sizeof(volmet[0]); i++) {
        char code[16];

        snprintf(code, sizeof(code), "VOLMET %s", volmet[i]);
        check(find(code, NULL) && find(code, NULL)->kind == AERO_VOLMET, "VOLMET area there", (long)i);
    }

    /* Where the CX8ANM list (the HFUnderground wiki's) differed from the Plan */
    check(has(find("NAT", NULL), 2899) && !has(find("NAT", NULL), 2889), "NAT 2899, not 2889", 0);
    check(has(find("CWP", NULL), 11384) && !has(find("CWP", NULL), 11304), "CWP 11384, not 11304", 0);
    check(has(find("MID", NULL), 2944) && has(find("MID", NULL), 3473) && has(find("MID", NULL), 4669), "MID", 0);
    check(size(find("AFI", NULL)) == 20 && has(find("AFI", NULL), 13294), "AFI's 20", 0);
    check(has(find("SAM", NULL), 10024) && !has(find("VOLMET SAM", NULL), 10024), "SAM 10024 a route's", 0);
    check(size(find("INO", NULL)) == 5, "INO", 0);
    check(size(find("NAT", NULL)) == 24, "NAT's 24", 0);
    check(has(find("VOLMET EUR", NULL), 3413) && has(find("VOLMET EUR", NULL), 8957) &&
              has(find("VOLMET EUR", NULL), 13264),
          "Shannon's three in VOLMET EUR", 0);
    check(has(find("VOLMET", "Shannon, Ireland"), 5505), "Shannon 5505", 0);
    check(has(find("VOLMET NAT", NULL), 3485) && has(find("VOLMET NAT", NULL), 6604) &&
              has(find("VOLMET NAT", NULL), 10051) && has(find("VOLMET NAT", NULL), 13270),
          "New York and Gander VOLMET", 0);
    check(size(find("SAR", NULL)) == 2 && has(find("SAR", NULL), 3023) && has(find("SAR", NULL), 5680), "SAR", 0);
    check(size(find("LDOC", NULL)) == 74, "LDOC's 74", size(find("LDOC", NULL)));

    /* The areas */
    int16_t  fam[600];
    uint16_t khz[600];
    int      n = aero_rows(AERO_AREA_ALL, fam, khz, 600);

    check(n == total, "All has every row", n);
    n = aero_rows(AERO_AREA_LDOC, fam, khz, 600);
    check(n == 74, "LDOC rows", n);
    for (int i = 0; i < n; i++) check(aero_family(fam[i])->kind == AERO_LDOC, "LDOC only", i);
    check(aero_rows(AERO_AREA_ALL, fam, khz, 10) == 10, "at most max", 0);

    n = aero_rows(AERO_AREA_EUR, fam, khz, 600);
    bool nat = false, car = false, sar = false, ldoc = false, shannon = false;
    for (int i = 0; i < n; i++) {
        const aero_family_t *f = aero_family(fam[i]);

        nat |= !strcmp(f->code, "NAT");
        car |= !strcmp(f->code, "CAR");
        sar |= !strcmp(f->code, "SAR");
        ldoc |= f->kind == AERO_LDOC;
        shannon |= khz[i] == 5505;
        check(i == 0 || fam[i] >= fam[i - 1], "rows family by family", i);
    }
    check(nat && sar && shannon && !car && !ldoc, "Europe: NAT, SAR, Shannon; not CAR, LDOC", n);
    check(aero_family(fam[0])->kind == AERO_VOLMET, "the weather first", 0);

    check(aero_area_for_region(1) == AERO_AREA_EUR && aero_area_for_region(2) == AERO_AREA_AMER &&
              aero_area_for_region(3) == AERO_AREA_ASIA,
          "area by region", 0);
    for (int a = 0; a < AERO_AREAS; a++) check(aero_area_name(a)[0] != 0, "area name", a);
}

int main(void) {
    test_aero_lists();
    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "OK", checks, fails);
    return fails != 0;
}
