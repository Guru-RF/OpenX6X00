/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  The HF aeronautical frequencies (aero.h). Where they come from:
 *
 *  - The routes (MWARA), the VOLMET allotment areas and the worldwide allotments: the ITU Radio Regulations, Appendix
 *    27 (Rev.WRC-19), Part II, Section II: the Plan by areas (No. 27/213), checked frequency by frequency against the
 *    Plan in numerical order (Nos. 27/218 to 27/231). Where the two differ the second is taken: 27/213 gives NAT
 *    2 889 kHz, which is no channel of the Plan (they are 3 kHz apart, from 2 851), and Article 2 allots 2 899 to NAT,
 *    the frequency Shanwick and Gander use.
 *  - 3023 and 5680 kHz: Article 3 (Nos. 27/232 to 27/238), for common use worldwide, search and rescue among it.
 *  - LDOC: the worldwide allotment areas I to V (Nos. 27/205 to 27/209), which No. 27/217 keeps for the aircraft
 *    operating agencies' own communication with their aircraft anywhere: the airlines' long-distance operational
 *    control stations. One list here, the five areas together.
 *  - The VOLMET stations outside those allotments: the EiBi list, as the radio's broadcast list has it
 *    (rootfs/usr/share/x6100/broadcast.csv): Shannon on 5505, the RAF on 11253, Ezeiza on 11369, Cordoba, Trenton.
 *    Not the RAF's 5450 kHz: that is the top edge of the 60 m band in ham_bands.c, where PTT goes out even with TX
 *    outside ham bands off, and in USB above the band's edge. Every frequency here is outside the amateur bands, so
 *    the switch decides as anywhere else (tools/aerocb checks every one).
 *  - USB: No. 27/57, J3E, with the dial on the carrier (reference) frequency (Nos. 27/11, 27/18); A3E and H3E only
 *    on 3023 and 5680 kHz, where No. 27/238 asks for the upper sideband too.
 *
 *  The areas a family is heard in are ours, for the list: NAT in Europe as well as in the Americas, SAT on both sides
 *  of the South Atlantic, the Pacific ones in the Americas and in Asia.
 *
 *  After condoruy's CX8ANM edition, whose list came from the HFUnderground wiki: it had CWP 11304 for 11384, lacked
 *  NAT 2899, three of MID's, eight of AFI's, SAM 10024 (under a South America VOLMET), the Indian Ocean (INO) and the
 *  LDOC allotments, and an Argentine VOLMET list of NAT frequencies.
 */

#include "aero.h"

#include <stddef.h>

#define F(name, ...) static const uint16_t name[] = {__VA_ARGS__, 0}

#define R1 AERO_IN(AERO_AREA_EUR)
#define R2 AERO_IN(AERO_AREA_AMER)
#define R3 AERO_IN(AERO_AREA_ASIA)

/* VOLMET allotment areas */
F(vafi, 2860, 3404, 5499, 6538, 8852, 10057, 13261);
F(vcar, 2950, 5580, 11315);
F(veur, 2998, 3413, 5640, 6580, 8957, 11378, 13264);
F(vmid, 2956, 5589, 8945, 11393);
F(vnat, 2905, 3485, 5592, 6604, 8870, 10051, 13270, 13276);
F(vnca, 3461, 4663, 5676, 10090, 13279);
F(vpac, 2863, 6679, 8828, 13282);
F(vsam, 2881, 5601, 10087, 13279);
F(vsea, 2965, 3458, 5673, 6676, 8849, 11387, 13285);

/* VOLMET stations outside them (EiBi) */
F(v_shannon, 5505);
F(v_raf, 11253);
F(v_ezeiza, 11369);
F(v_cordoba, 3434, 5475);
F(v_trenton, 6754, 15034);

/* Major world air route areas */
F(afi, 2851, 2878, 3419, 3425, 3467, 4657, 5493, 5652, 5658, 6559, 6574, 6673, 8894, 8903, 11300, 11330, 13273, 13288,
  13294, 17961);
F(car, 2887, 3455, 5520, 5550, 6577, 6586, 8846, 8918, 11387, 11396, 13297, 17907);
F(cep, 2869, 3413, 4657, 5547, 5574, 6673, 8843, 10057, 11282, 13300, 17904);
F(cwp, 2998, 3455, 4666, 5652, 5661, 6532, 6562, 8903, 10081, 11384, 13300, 17904);
F(ea, 3016, 3485, 3491, 5655, 5670, 6571, 8897, 10042, 11396, 13297, 13303, 13309, 17907);
F(eur, 3479, 5661, 6598, 10084, 13288, 17961);
F(ino, 3476, 5634, 8879, 13306, 17961);
F(mid, 2944, 2992, 3467, 3473, 4669, 5658, 5667, 6625, 6631, 8918, 8951, 10018, 11375, 13288, 13312, 17961);
F(nat, 2872, 2899, 2962, 2971, 3016, 3476, 4675, 5598, 5616, 5649, 6622, 6628, 8825, 8831, 8864, 8879, 8891, 8906,
  11279, 11309, 11336, 13291, 13306, 17946);
F(nca, 3004, 3019, 4678, 5646, 5664, 6592, 10096, 13303, 13315, 17958);
F(np, 2932, 5628, 6655, 6661, 10048, 11330, 13300, 17904);
F(sam, 2944, 3479, 4669, 5526, 6649, 8855, 10024, 10096, 11360, 13297, 17907);
F(sat, 2854, 2935, 3452, 5565, 6535, 8861, 11291, 13315, 13357, 17955);
F(sea, 3470, 3485, 5649, 5655, 6556, 8942, 10066, 11396, 13309, 13318, 17907);
F(sp, 3467, 5559, 5643, 8867, 10084, 11327, 13300, 17904);

/* Worldwide */
F(common, 3023, 5680);
F(ldoc, 3007, 3010, 3013, 3494, 3497, 4654, 4687, 5529, 5532, 5535, 5538, 5541, 5544, 6637, 6640, 6643, 6646, 8921,
  8924, 8927, 8930, 8933, 8936, 10027, 10030, 10033, 10069, 10072, 10075, 10078, 11342, 11345, 11348, 11351, 11354,
  13324, 13327, 13330, 13333, 13336, 13339, 13342, 13345, 13348, 13351, 17916, 17919, 17922, 17925, 17928, 17931,
  17934, 17937, 17940, 21940, 21943, 21946, 21949, 21952, 21955, 21958, 21961, 21964, 21967, 21970, 21973, 21976,
  21979, 21982, 21985, 21988, 21991, 21994, 21997);

/* In the order of the list: the weather first, then the routes, then the worldwide ones */
static const aero_family_t fam[] = {
    {"VOLMET AFI", "Africa, Indian Ocean", AERO_VOLMET, R1, vafi},
    {"VOLMET CAR", "Caribbean", AERO_VOLMET, R2, vcar},
    {"VOLMET EUR", "Europe", AERO_VOLMET, R1, veur},
    {"VOLMET MID", "Middle East", AERO_VOLMET, R1, vmid},
    {"VOLMET NAT", "North Atlantic", AERO_VOLMET, R1 | R2, vnat},
    {"VOLMET NCA", "North Central Asia", AERO_VOLMET, R1 | R3, vnca},
    {"VOLMET PAC", "Pacific", AERO_VOLMET, R2 | R3, vpac},
    {"VOLMET SAM", "South America", AERO_VOLMET, R2, vsam},
    {"VOLMET SEA", "South East Asia", AERO_VOLMET, R3, vsea},
    {"VOLMET", "Shannon, Ireland", AERO_VOLMET, R1, v_shannon},
    {"VOLMET", "RAF, United Kingdom (military)", AERO_VOLMET, R1, v_raf},
    {"VOLMET", "Ezeiza, Argentina", AERO_VOLMET, R2, v_ezeiza},
    {"VOLMET", "Cordoba, Argentina", AERO_VOLMET, R2, v_cordoba},
    {"VOLMET", "Trenton, Canada (military)", AERO_VOLMET, R2, v_trenton},
    {"AFI", "Africa", AERO_MWARA, R1, afi},
    {"CAR", "Caribbean", AERO_MWARA, R2, car},
    {"CEP", "Central East Pacific", AERO_MWARA, R2 | R3, cep},
    {"CWP", "Central West Pacific", AERO_MWARA, R3, cwp},
    {"EA", "East Asia", AERO_MWARA, R3, ea},
    {"EUR", "Europe", AERO_MWARA, R1, eur},
    {"INO", "Indian Ocean", AERO_MWARA, R1 | R3, ino},
    {"MID", "Middle East", AERO_MWARA, R1, mid},
    {"NAT", "North Atlantic", AERO_MWARA, R1 | R2, nat},
    {"NCA", "North Central Asia", AERO_MWARA, R1 | R3, nca},
    {"NP", "North Pacific", AERO_MWARA, R2 | R3, np},
    {"SAM", "South America", AERO_MWARA, R2, sam},
    {"SAT", "South Atlantic", AERO_MWARA, R1 | R2, sat},
    {"SEA", "South East Asia", AERO_MWARA, R3, sea},
    {"SP", "South Pacific", AERO_MWARA, R2 | R3, sp},
    {"SAR", "Common, search and rescue", AERO_COMMON, R1 | R2 | R3, common},
    {"LDOC", "Airline operational control", AERO_LDOC, 0, ldoc},
};

int aero_families(void) {
    return (int)(sizeof(fam) / sizeof(fam[0]));
}

const aero_family_t *aero_family(int i) {
    return i >= 0 && i < aero_families() ? &fam[i] : NULL;
}

bool aero_shown(const aero_family_t *f, int area) {
    if (!f) return false;
    if (area == AERO_AREA_ALL) return true;
    if (area == AERO_AREA_LDOC) return f->kind == AERO_LDOC;
    return f->kind != AERO_LDOC && (f->areas & AERO_IN(area));
}

const char *aero_area_name(int area) {
    switch (area) {
        case AERO_AREA_EUR:  return "Europe, Africa, Middle East";
        case AERO_AREA_AMER: return "Americas";
        case AERO_AREA_ASIA: return "Asia and Pacific";
        case AERO_AREA_LDOC: return "Worldwide LDOC";
        default:             return "All areas";
    }
}

int aero_area_for_region(int region) {
    return region == 2 ? AERO_AREA_AMER : region == 3 ? AERO_AREA_ASIA : AERO_AREA_EUR;
}

int aero_rows(int area, int16_t *rows_fam, uint16_t *rows_khz, int max) {
    int n = 0;

    for (int i = 0; i < aero_families(); i++) {
        if (!aero_shown(&fam[i], area)) continue;
        for (const uint16_t *k = fam[i].khz; *k && n < max; k++) {
            rows_fam[n] = (int16_t)i;
            rows_khz[n] = *k;
            n++;
        }
    }
    return n;
}
