/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of src/cb.c and src/aero.c, the tables of the CB and HF Air Band apps.
 *
 *  CB: the 40 channels written out here again (the FCC's and BNetzA's tables), each plan's channel to frequency at
 *  its edges and its odd places (23 between 22 and 24, Germany's 41-80 below 1, the UK's 1.25 kHz, Poland's 5 kHz,
 *  the export grid, E ending below 10 m), every channel back from its frequency, the modes per plan and channel
 *  (Germany's 41-80 FM, Australia's no FM, 11 AM, 16 LSB), the knob's steps by channel number, the plan by region and
 *  by country, the names. HF aero: every frequency of the ITU Plan's families on a channel of its raster (Appendix 27,
 *  No. 27/18), ascending, the places the CX8ANM list had wrong, the areas. Both: no frequency of either app is in an
 *  amateur band, so main's rule decides PTT there as anywhere outside them: refused with TX outside ham bands off,
 *  allowed with it on (tx_ranges_check; aerocb_tx_test.cpp does the same through the real cfg_tx_refusal).
 */

#include "aero.h"
#include "cb.h"
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

/* Channels 1-40, kHz, as 47 CFR 95.963 and Vfg. 21/2021 list them */
static const int std40[40] = {
    26965, 26975, 26985, 27005, 27015, 27025, 27035, 27055, 27065, 27075, 27085, 27105, 27115, 27125,
    27135, 27155, 27165, 27175, 27185, 27205, 27215, 27225, 27255, 27235, 27245, 27265, 27275, 27285,
    27295, 27305, 27315, 27325, 27335, 27345, 27355, 27365, 27375, 27385, 27395, 27405,
};

/* Outside the amateur bands, where main's rule goes by TX outside ham bands: off refused, on allowed */
static bool off_band(int32_t hz) {
    return tx_ranges_check(TX_RANGES_BANDS, hz, true, 0, false) == HAM_TX_OFF_BAND &&
           tx_ranges_check(TX_RANGES_BANDS, hz, true, 0, true) == HAM_TX_OK;
}

static void test_cb_freqs(void) {
    static const int count[CB_PLANS] = {40, 80, 40, 40, 40, 40, 40, 40, 40, 11};

    for (int p = 0; p < CB_PLANS; p++) check(cb_channels(p) == count[p], "channels of plan", p);
    check(cb_channels(-1) == 0 && cb_channels(CB_PLANS) == 0, "no plan", 0);

    for (int ch = 1; ch <= 40; ch++) {
        check(cb_freq(CB_PLAN_CEPT, ch) == std40[ch - 1] * 1000, "CEPT channel", ch);
        check(cb_freq(CB_PLAN_US, ch) == std40[ch - 1] * 1000, "US channel", ch);
        check(cb_freq(CB_PLAN_AU, ch) == std40[ch - 1] * 1000, "AU channel", ch);
        check(cb_freq(CB_PLAN_DE, ch) == std40[ch - 1] * 1000, "DE channel", ch);
        check(cb_freq(CB_PLAN_PL, ch) == std40[ch - 1] * 1000 - 5000, "PL channel", ch);
        check(cb_freq(CB_PLAN_A, ch) == std40[ch - 1] * 1000 - 900000, "A channel", ch);
        check(cb_freq(CB_PLAN_B, ch) == std40[ch - 1] * 1000 - 450000, "B channel", ch);
        check(cb_freq(CB_PLAN_D, ch) == std40[ch - 1] * 1000 + 450000, "D channel", ch);
        check(cb_freq(CB_PLAN_UK, ch) == 27601250 + (ch - 1) * 10000, "UK channel", ch);
    }
    check(cb_freq(CB_PLAN_CEPT, 23) == 27255000 && cb_freq(CB_PLAN_CEPT, 24) == 27235000, "23 above 24", 0);
    check(cb_freq(CB_PLAN_CEPT, 19) == 27185000, "CEPT 19", cb_freq(CB_PLAN_CEPT, 19));
    check(cb_freq(CB_PLAN_PL, 19) == 27180000, "PL 19", cb_freq(CB_PLAN_PL, 19));
    check(cb_freq(CB_PLAN_UK, 1) == 27601250 && cb_freq(CB_PLAN_UK, 40) == 27991250, "UK edges", 0);
    check(cb_freq(CB_PLAN_DE, 41) == 26565000, "DE 41", cb_freq(CB_PLAN_DE, 41));
    check(cb_freq(CB_PLAN_DE, 61) == 26765000, "DE 61", cb_freq(CB_PLAN_DE, 61));
    check(cb_freq(CB_PLAN_DE, 80) == 26955000, "DE 80", cb_freq(CB_PLAN_DE, 80));
    for (int ch = 41; ch <= 80; ch++) check(cb_freq(CB_PLAN_DE, ch) == 26565000 + (ch - 41) * 10000, "DE 41-80", ch);
    check(cb_freq(CB_PLAN_A, 1) == 26065000 && cb_freq(CB_PLAN_D, 40) == 27855000, "export edges", 0);
    check(cb_freq(CB_PLAN_E, 1) == 27865000 && cb_freq(CB_PLAN_E, 11) == 27985000, "E 1 and 11", 0);
    check(cb_freq(CB_PLAN_E, 12) == 0, "no E 12 (28.005 MHz)", cb_freq(CB_PLAN_E, 12));
    check(cb_freq(CB_PLAN_CEPT, 0) == 0 && cb_freq(CB_PLAN_CEPT, 41) == 0 && cb_freq(CB_PLAN_UK, 41) == 0, "no channel",
          0);

    for (int p = 0; p < CB_PLANS; p++) {
        for (int ch = 1; ch <= cb_channels(p); ch++) {
            int32_t f = cb_freq(p, ch);

            check(f > 26000000 && f < 28000000, "between 26 MHz and 10 m", f);
            check(cb_channel_at(p, f) == ch, "channel back from its frequency", p * 100 + ch);
            check(cb_channel_at(p, f + 1000) == ch && cb_channel_at(p, f - 1000) == ch, "within 1 kHz", p * 100 + ch);
            check(cb_channel_at(p, f + 1001) != ch, "past 1 kHz", p * 100 + ch);
            check(cb_in_plan(p, f), "in the plan", p * 100 + ch);
            for (int c2 = ch + 1; c2 <= cb_channels(p); c2++) check(cb_freq(p, c2) != f, "two on one frequency", f);
            check(off_band(f), "CB channel in an amateur band", f);
        }
    }
    check(cb_in_plan(CB_PLAN_CEPT, 26960000) && !cb_in_plan(CB_PLAN_CEPT, 26959999), "CEPT lower edge", 0);
    check(cb_in_plan(CB_PLAN_CEPT, 27410000) && !cb_in_plan(CB_PLAN_CEPT, 27410001), "CEPT upper edge", 0);
    check(cb_in_plan(CB_PLAN_DE, 26560000) && !cb_in_plan(CB_PLAN_CEPT, 26560000), "DE goes lower", 0);
    check(!cb_in_plan(CB_PLAN_CEPT, 28000000) && !cb_in_plan(CB_PLAN_E, 28000000), "not 10 m", 0);
}

static void test_cb_modes(void) {
    const unsigned all = CB_MODE_BIT(CB_AM) | CB_MODE_BIT(CB_FM) | CB_MODE_BIT(CB_USB) | CB_MODE_BIT(CB_LSB);

    check(cb_plan_mode(CB_PLAN_CEPT) == CB_FM && cb_plan_modes(CB_PLAN_CEPT) == all, "CEPT: FM first, all", 0);
    check(cb_plan_mode(CB_PLAN_DE) == CB_FM, "DE: FM first", 0);
    check(cb_plan_modes(CB_PLAN_UK) == CB_MODE_BIT(CB_FM), "UK: FM only", 0);
    check(cb_plan_mode(CB_PLAN_US) == CB_AM && cb_plan_modes(CB_PLAN_US) == all, "US: AM first, FM allowed", 0);
    check(cb_plan_mode(CB_PLAN_AU) == CB_AM && !(cb_plan_modes(CB_PLAN_AU) & CB_MODE_BIT(CB_FM)), "AU: AM, no FM", 0);
    check(cb_plan_mode(CB_PLAN_PL) == CB_AM, "PL: AM first", 0);

    for (int ch = 1; ch <= 40; ch++) check(cb_channel_modes(CB_PLAN_DE, ch) == all, "DE 1-40 all modes", ch);
    for (int ch = 41; ch <= 80; ch++) {
        check(cb_channel_modes(CB_PLAN_DE, ch) == CB_MODE_BIT(CB_FM), "DE 41-80 FM only", ch);
        check(cb_channel_mode(CB_PLAN_DE, ch, CB_AM) == CB_FM, "DE 41-80 heard in FM", ch);
    }
    check(cb_channel_modes(CB_PLAN_AU, 11) == CB_MODE_BIT(CB_AM), "AU 11 AM only", 0);
    check(cb_channel_modes(CB_PLAN_AU, 16) == CB_MODE_BIT(CB_LSB), "AU 16 LSB only", 0);
    check(cb_channel_mode(CB_PLAN_AU, 16, CB_AM) == CB_LSB, "AU 16 in LSB", 0);
    check(cb_channel_mode(CB_PLAN_AU, 11, CB_USB) == CB_AM, "AU 11 in AM", 0);
    check(cb_channel_mode(CB_PLAN_AU, 5, CB_FM) == CB_AM, "AU FM falls back to AM", 0);
    check(cb_channel_mode(CB_PLAN_AU, 5, CB_USB) == CB_USB, "AU USB kept", 0);
    check(cb_channel_mode(CB_PLAN_UK, 9, CB_AM) == CB_FM, "UK always FM", 0);
    check(cb_channel_mode(CB_PLAN_CEPT, 5, CB_LSB) == CB_LSB, "CEPT LSB kept", 0);
    check(cb_channel_mode(CB_PLAN_CEPT, 5, CB_MODES) == CB_FM, "CEPT no choice: FM", 0);
    check(cb_channel_modes(CB_PLAN_CEPT, 41) == 0, "no channel, no modes", 0);

    check(!strcmp(cb_channel_note(CB_PLAN_US, 9), "Emergency, travellers"), "US 9", 0);
    check(!strcmp(cb_channel_note(CB_PLAN_AU, 16), "Calling, LSB"), "AU 16", 0);
    check(!strcmp(cb_channel_note(CB_PLAN_DE, 50), "FM only"), "DE 50", 0);
    check(!strcmp(cb_channel_note(CB_PLAN_DE, 19), "Road"), "DE 19", 0);
    check(!strcmp(cb_channel_note(CB_PLAN_D, 9), ""), "export 9: nothing", 0);
}

static void test_cb_steps(void) {
    int32_t f = cb_freq(CB_PLAN_CEPT, 22);

    f = cb_step(CB_PLAN_CEPT, f, 1);
    check(f == 27255000, "22 + 1 is 23", f);
    f = cb_step(CB_PLAN_CEPT, f, 1);
    check(f == 27235000, "23 + 1 is 24", f);
    f = cb_step(CB_PLAN_CEPT, f, -2);
    check(f == 27225000, "24 - 2 is 22", f);
    check(cb_step(CB_PLAN_CEPT, 27405000, 1) == 27405000, "40 stays", 0);
    check(cb_step(CB_PLAN_CEPT, 26965000, -3) == 26965000, "1 stays", 0);
    check(cb_step(CB_PLAN_CEPT, 27180000, 1) == 27185000, "off a channel, up: the next", 0);
    check(cb_step(CB_PLAN_CEPT, 27180000, -1) == 27175000, "off a channel, down: the one below", 0);
    check(cb_step(CB_PLAN_CEPT, 27180000, 3) == 27215000, "off a channel, 3 up: 21", cb_step(CB_PLAN_CEPT, 27180000, 3));
    check(cb_step(CB_PLAN_CEPT, 28100000, 1) == 28100000, "above them all: nowhere up", 0);
    check(cb_step(CB_PLAN_CEPT, 28100000, -1) == 27405000, "above them all, down: 40", 0);
    check(cb_step(CB_PLAN_DE, 27405000, 1) == 26565000, "DE 40 + 1 is 41", 0);
    check(cb_step(CB_PLAN_DE, 26955000, 1) == 26955000, "DE 80 stays", 0);
    check(cb_step(CB_PLAN_UK, 27781250, 1) == 27791250, "UK 19 + 1", 0);
    check(cb_step(CB_PLAN_E, 27985000, 1) == 27985000, "E stops below 10 m", 0);
    check(cb_step(CB_PLAN_CEPT, 27185000, 0) == 27185000, "no step", 0);
}

static void test_cb_plans(void) {
    char buf[24];

    check(cb_plan_for_region(1) == CB_PLAN_CEPT, "region 1", 0);
    check(cb_plan_for_region(2) == CB_PLAN_US, "region 2", 0);
    check(cb_plan_for_region(3) == CB_PLAN_AU, "region 3", 0);
    check(cb_plan_for_region(0) == CB_PLAN_CEPT, "no region", 0);
    check(cb_plan_for_country("DE") == CB_PLAN_DE && cb_plan_for_country("de") == CB_PLAN_DE, "Germany", 0);
    check(cb_plan_for_country("GB") == CB_PLAN_UK, "United Kingdom", 0);
    check(cb_plan_for_country("PL") == CB_PLAN_PL, "Poland", 0);
    check(cb_plan_for_country("AU") == CB_PLAN_AU, "Australia", 0);
    check(cb_plan_for_country("US") == CB_PLAN_US && cb_plan_for_country("CA") == CB_PLAN_US, "USA, Canada", 0);
    check(cb_plan_for_country("FR") == -1 && cb_plan_for_country("UY") == -1, "the region's", 0);
    check(cb_plan_for_country("") == -1 && cb_plan_for_country(NULL) == -1 && cb_plan_for_country("DEU") == -1,
          "no country", 0);

    check(!strcmp(cb_name(CB_PLAN_CEPT, 19, buf, sizeof(buf)), "CB 19"), "CB 19", 0);
    check(!strcmp(cb_name(CB_PLAN_UK, 19, buf, sizeof(buf)), "UK 19"), "UK 19", 0);
    check(!strcmp(cb_name(CB_PLAN_PL, 19, buf, sizeof(buf)), "PL 19"), "PL 19", 0);
    check(!strcmp(cb_name(CB_PLAN_D, 19, buf, sizeof(buf)), "CB D-19"), "CB D-19", 0);
    check(!strcmp(cb_name(CB_PLAN_DE, 61, buf, sizeof(buf)), "CB 61"), "CB 61", 0);
    check(!strcmp(cb_freq_text(27185000, buf, sizeof(buf)), "27.185"), "27.185", 0);
    check(!strcmp(cb_freq_text(27601250, buf, sizeof(buf)), "27.60125"), "27.60125", 0);
    check(!strcmp(cb_freq_text(26565000, buf, sizeof(buf)), "26.565"), "26.565", 0);
    check(!strcmp(cb_mode_name(CB_LSB), "LSB") && !strcmp(cb_mode_name(CB_MODES), ""), "mode names", 0);
    for (int p = 0; p < CB_PLANS; p++) check(cb_plan_name(p)[0] != 0, "plan name", p);
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
    test_cb_freqs();
    test_cb_modes();
    test_cb_steps();
    test_cb_plans();
    test_aero_lists();
    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "OK", checks, fails);
    return fails != 0;
}
