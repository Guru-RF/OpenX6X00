/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  The CB channel plans (cb.h). Where each comes from:
 *
 *  - The 40 channels, 26.965 to 27.405 MHz, 23 between 22 and 24 out of order: the FCC's table (47 CFR 95.963), the
 *    same in ECC/DEC/(11)03, Germany's Vfg. 21/2021 and Australia's class licence below.
 *  - Europe (CEPT): ECC Decision (11)03 of 24 June 2011, amended 17 June 2016: 26.960-27.410 MHz, the 40 channels in
 *    FM (angle modulation), AM (double sideband) and SSB. FM is what most of Europe uses.
 *  - Germany: BNetzA Vfg. 21/2021 (Allgemeinzuteilung CB-Funk, until 31.12.2030): the 40, and channels 41 to 80 at
 *    26.565 to 26.955 MHz, 10 kHz apart; FM on 1 to 80, AM and SSB on 1 to 40 only.
 *  - United Kingdom 27/81: Ofcom IR 2027: 27.60125 to 27.99125 MHz, 10 kHz apart, FM only; Ofcom's 2014 statement
 *    kept them FM only when it allowed AM and SSB on the CEPT channels (the Europe plan) from 27 June 2014.
 *  - USA and the Americas: 47 CFR 95.971 (AM, SSB, and FM as an option), 95.931(a)(2): channel 9 only for emergency
 *    communications or traveler assistance. Channel 19 for the road is a custom, not a rule. Canada and Mexico use
 *    the same 40.
 *  - Australia: Radiocommunications (Citizen Band Radio Stations) Class Licence 2015, Schedule 1: AM or SSB (USB or
 *    LSB) on the HF channels, no FM; channel 9 for emergencies only, 11 AM and 16 LSB only, both to call.
 *  - Poland: 26.960-27.410 MHz without a licence (regulation of 12 December 2014); Polish sets' "PL" channels sit
 *    5 kHz below the CEPT ones (19 at 27.180 MHz) and AM is the custom.
 *  - The export grid: the 40 moved by 450 kHz steps, as export sets have them, named as in Eastern Europe (C the 40
 *    above; on six-band sets the same 40 are called D). No country's CB plan: tuned as the others, and TX there
 *    goes by the radio's rule as anywhere. E stops below 28.000 MHz, the 10 m band.
 *
 *  Taken from condoruy's CX8ANM edition (the 40, the UK band, the export grid and the knob's channel steps), with the
 *  plans per country and their modes added here.
 */

#include "cb.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

#define ALL_MODES (CB_MODE_BIT(CB_AM) | CB_MODE_BIT(CB_FM) | CB_MODE_BIT(CB_USB) | CB_MODE_BIT(CB_LSB))
#define AM_SSB    (CB_MODE_BIT(CB_AM) | CB_MODE_BIT(CB_USB) | CB_MODE_BIT(CB_LSB))
#define FM_ONLY   CB_MODE_BIT(CB_FM)

/* Channels 1 to 40, kHz */
static const int32_t std_khz[40] = {
    26965, 26975, 26985, 27005, 27015, 27025, 27035, 27055, 27065, 27075,
    27085, 27105, 27115, 27125, 27135, 27155, 27165, 27175, 27185, 27205,
    27215, 27225, 27255, 27235, 27245, 27265, 27275, 27285, 27295, 27305,
    27315, 27325, 27335, 27345, 27355, 27365, 27375, 27385, 27395, 27405,
};

static const struct {
    const char *name;
    const char *prefix;     /* of a channel's name */
    int         count;
    int32_t     offset;     /* Hz from the 40 */
    unsigned    modes;
    cb_mode_t   mode;
} plans[CB_PLANS] = {
    [CB_PLAN_CEPT] = {"Europe (CEPT)", "CB ", 40, 0, ALL_MODES, CB_FM},
    [CB_PLAN_DE]   = {"Germany (80 channels)", "CB ", 80, 0, ALL_MODES, CB_FM},
    [CB_PLAN_UK]   = {"United Kingdom 27/81", "UK ", 40, 0, FM_ONLY, CB_FM},
    [CB_PLAN_US]   = {"USA, Americas (FCC)", "CB ", 40, 0, ALL_MODES, CB_AM},
    [CB_PLAN_AU]   = {"Australia", "CB ", 40, 0, AM_SSB, CB_AM},
    [CB_PLAN_PL]   = {"Poland (PL)", "PL ", 40, -5000, ALL_MODES, CB_AM},
    [CB_PLAN_A]    = {"Band A (export)", "CB A-", 40, -900000, ALL_MODES, CB_AM},
    [CB_PLAN_B]    = {"Band B (export)", "CB B-", 40, -450000, ALL_MODES, CB_AM},
    [CB_PLAN_D]    = {"Band D (export)", "CB D-", 40, 450000, ALL_MODES, CB_AM},
    [CB_PLAN_E]    = {"Band E (export)", "CB E-", 11, 900000, ALL_MODES, CB_AM},    /* E12 would be 28.005 MHz */
};

static bool valid(int plan, int ch) {
    return plan >= 0 && plan < CB_PLANS && ch >= 1 && ch <= plans[plan].count;
}

int cb_channels(int plan) {
    return plan >= 0 && plan < CB_PLANS ? plans[plan].count : 0;
}

int32_t cb_freq(int plan, int ch) {
    if (!valid(plan, ch)) return 0;
    if (plan == CB_PLAN_UK) return 27601250 + (ch - 1) * 10000;
    if (ch > 40) return 26565000 + (ch - 41) * 10000;    /* Germany's 41-80 */
    return std_khz[ch - 1] * 1000 + plans[plan].offset;
}

const char *cb_plan_name(int plan) {
    return plan >= 0 && plan < CB_PLANS ? plans[plan].name : "";
}

unsigned cb_plan_modes(int plan) {
    return plan >= 0 && plan < CB_PLANS ? plans[plan].modes : 0;
}

cb_mode_t cb_plan_mode(int plan) {
    return plan >= 0 && plan < CB_PLANS ? plans[plan].mode : CB_AM;
}

unsigned cb_channel_modes(int plan, int ch) {
    if (!valid(plan, ch)) return 0;
    if (plan == CB_PLAN_DE && ch > 40) return FM_ONLY;
    if (plan == CB_PLAN_AU && ch == 11) return CB_MODE_BIT(CB_AM);
    if (plan == CB_PLAN_AU && ch == 16) return CB_MODE_BIT(CB_LSB);
    return plans[plan].modes;
}

cb_mode_t cb_channel_mode(int plan, int ch, cb_mode_t want) {
    unsigned m = cb_channel_modes(plan, ch);

    if (want < CB_MODES && (m & CB_MODE_BIT(want))) return want;
    if (m & CB_MODE_BIT(cb_plan_mode(plan))) return cb_plan_mode(plan);
    for (int k = 0; k < CB_MODES; k++)
        if (m & CB_MODE_BIT(k)) return (cb_mode_t)k;
    return cb_plan_mode(plan);
}

const char *cb_channel_note(int plan, int ch) {
    if (!valid(plan, ch)) return "";
    switch (plan) {
        case CB_PLAN_US:
            return ch == 9 ? "Emergency, travellers" : ch == 19 ? "Highway" : "";
        case CB_PLAN_AU:
            return ch == 9 ? "Emergency only" : ch == 11 ? "Calling, AM" : ch == 16 ? "Calling, LSB" : "";
        case CB_PLAN_DE:
            if (ch > 40) return "FM only";
            /* fall through */
        case CB_PLAN_CEPT:
        case CB_PLAN_UK:
        case CB_PLAN_PL:
            return ch == 9 ? "Emergency" : ch == 19 ? "Road" : "";
        default:
            return "";
    }
}

int cb_channel_at(int plan, int32_t hz) {
    for (int ch = 1; ch <= cb_channels(plan); ch++)
        if (abs(hz - cb_freq(plan, ch)) <= 1000) return ch;
    return 0;
}

bool cb_in_plan(int plan, int32_t hz) {
    int32_t lo = 0, hi = 0;

    for (int ch = 1; ch <= cb_channels(plan); ch++) {
        int32_t f = cb_freq(plan, ch);

        if (!lo || f < lo) lo = f;
        if (f > hi) hi = f;
    }
    return lo && hz >= lo - 5000 && hz <= hi + 5000;
}

int32_t cb_step(int plan, int32_t hz, int steps) {
    int n  = cb_channels(plan);
    int ch = cb_channel_at(plan, hz);

    if (!n || !steps) return hz;
    if (!ch) {
        for (int c = 1; c <= n; c++) {
            int32_t f = cb_freq(plan, c), best = ch ? cb_freq(plan, ch) : 0;

            if (steps > 0 ? f > hz && (!best || f < best) : f < hz && (!best || f > best)) ch = c;
        }
        if (!ch) return hz;
        steps += steps > 0 ? -1 : 1;    /* that one was the first step */
    }
    ch += steps;
    ch = ch < 1 ? 1 : ch > n ? n : ch;
    return cb_freq(plan, ch);
}

int cb_plan_for_region(int region) {
    return region == 2 ? CB_PLAN_US : region == 3 ? CB_PLAN_AU : CB_PLAN_CEPT;
}

int cb_plan_for_country(const char *cc) {
    static const struct {
        char cc[3];
        int  plan;
    } own[] = {
        {"DE", CB_PLAN_DE}, {"GB", CB_PLAN_UK}, {"PL", CB_PLAN_PL}, {"AU", CB_PLAN_AU},
        {"US", CB_PLAN_US}, {"CA", CB_PLAN_US}, {"MX", CB_PLAN_US},
    };

    if (!cc || !cc[0] || !cc[1]) return -1;
    for (size_t i = 0; i < sizeof(own) / sizeof(own[0]); i++)
        if (toupper((unsigned char)cc[0]) == own[i].cc[0] && toupper((unsigned char)cc[1]) == own[i].cc[1] && !cc[2])
            return own[i].plan;
    return -1;
}

const char *cb_name(int plan, int ch, char *buf, size_t size) {
    if (!valid(plan, ch)) snprintf(buf, size, "CB");
    else snprintf(buf, size, "%s%d", plans[plan].prefix, ch);
    return buf;
}

const char *cb_freq_text(int32_t hz, char *buf, size_t size) {
    if (hz % 1000) snprintf(buf, size, "%d.%05d", (int)(hz / 1000000), (int)(hz % 1000000 / 10));
    else snprintf(buf, size, "%d.%03d", (int)(hz / 1000000), (int)(hz / 1000 % 1000));
    return buf;
}

const char *cb_mode_name(cb_mode_t mode) {
    static const char *const names[CB_MODES] = {"AM", "FM", "USB", "LSB"};

    return mode < CB_MODES ? names[mode] : "";
}
