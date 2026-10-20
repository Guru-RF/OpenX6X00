/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - HF/MF DSC receiver (ITU-R M.493), see dsc.h
 */

#include "dsc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define POS_FMT     12              /* stream position of the first format specifier (DX) */
#define RX_DELAY    5               /* the RX copy of a character comes 5 positions after the DX one */
#define NPOS        (int)(sizeof(((dsc_rx_t *)0)->sym) / sizeof(short))
#define MAX_BAD     3               /* more characters lost in both copies: noise, dropped */

/* DX 125 at 0, 2..10, RX 111..104 at 1, 3..15; -1 elsewhere */
static int phasing(int p) {
    if (p < 0 || p > 15)
        return -1;
    if (p % 2)
        return 111 - p / 2;
    return p <= 10 ? 125 : -1;
}

/* The k-th last symbol (10 bits ending 10 k bits ago), -1 when its check bits are wrong */
static int symbol(const dsc_rx_t *rx, int k) {
    int info = 0, check = 0;

    for (int j = 0; j < 10; j++) {
        int i = rx->nbits - 1 - (10 * k + 9 - j);
        int b = rx->bits[(unsigned)i % sizeof(rx->bits)];
        if (j < 7)
            info |= b << j;
        else
            check = check << 1 | b;
    }
    return check == 7 - __builtin_popcount(info) ? info : -1;
}

/* Character j of the call (0, 1 format specifiers): the DX copy, else the RX one */
static int char_at(const dsc_rx_t *rx, int j) {
    int d = POS_FMT + 2 * j, r = d + RX_DELAY;

    if (d < NPOS && rx->sym[d] >= 0)
        return rx->sym[d];
    return r < NPOS ? rx->sym[r] : -1;
}

/* n symbols as 2 n digits ('?' for a lost one) */
static void digits(const int *c, int n, char *out) {
    for (int i = 0; i < n; i++) {
        if (c[i] >= 0 && c[i] <= 99) {
            out[2 * i]     = '0' + c[i] / 10;
            out[2 * i + 1] = '0' + c[i] % 10;
        } else {
            out[2 * i] = out[2 * i + 1] = '?';
        }
    }
    out[2 * n] = 0;
}

static void mmsi(const int *c, char *out) {
    char d[11];

    digits(c, 5, d);
    memcpy(out, d, 9);   /* the 10th digit is 0 */
    out[9] = 0;
}

static void position(const int *c, char *out, size_t size) {
    static const char ns[] = "NNSS", ew[] = "EWEW";
    char              d[11];

    digits(c, 5, d);
    if (!strcmp(d, "9999999999") || d[0] < '0' || d[0] > '3') {
        out[0] = 0;
        return;
    }
    snprintf(out, size, "%.2s\xc2\xb0%.2s'%c %.3s\xc2\xb0%.2s'%c", d + 1, d + 3, ns[d[0] - '0'], d + 5, d + 8,
             ew[d[0] - '0']);
}

static void utc(const int *c, char *out) {
    char d[5];

    digits(c, 2, d);
    if (!strcmp(d, "8888"))
        out[0] = 0;
    else
        sprintf(out, "%.2s:%.2s", d, d + 2);
}

/* Area address: quadrant, latitude, longitude of the NW corner, then its extent in degrees */
static void area(const int *c, char *out, size_t size) {
    static const char ns[] = "NNSS", ew[] = "EWEW";
    char              d[11];

    digits(c, 5, d);
    if (d[0] < '0' || d[0] > '3')
        snprintf(out, size, "area %s", d);
    else
        snprintf(out, size, "%.2s%c %.3s%c %.2sx%.3s", d + 1, ns[d[0] - '0'], d + 3, ew[d[0] - '0'], d + 6, d + 8);
}

/* Working frequency: 3 symbols, 0-2 first: the frequency in 100 Hz; 3: an HF/MF channel */
static void frequency(const int *c, char *out, size_t size) {
    char d[7];

    out[0] = 0;
    if (c[0] == 126 || c[0] < 0 || c[0] > 99)
        return;
    digits(c, 3, d);
    if (strchr(d, '?'))
        return;
    long v = atol(d);
    if (d[0] <= '2') {
        if (v % 10)
            snprintf(out, size, "%ld.%ld kHz", v / 10, v % 10);
        else
            snprintf(out, size, "%ld kHz", v / 10);
    } else if (d[0] == '3') {
        snprintf(out, size, "ch %s", d + 1);
    }
}

static bool is_eos(int s) {
    return s == 117 || s == 122 || s == 127;
}

/* The EOS at character e: its DX or RX copy, and one more of them or of the 2 DX after the ECC
 * (the ECC itself may be 117, 122 or 127) */
static int eos_at(const dsc_rx_t *rx, int e) {
    int v[4] = {rx->sym[POS_FMT + 2 * e], rx->sym[POS_FMT + 2 * e + RX_DELAY], rx->sym[POS_FMT + 2 * (e + 2)],
                rx->sym[POS_FMT + 2 * (e + 3)]};

    for (int i = 0; i < 2; i++) {
        if (!is_eos(v[i]))
            continue;
        int n = 0;
        for (int k = 0; k < 4; k++)
            n += v[k] == v[i];
        if (n >= 2)
            return v[i];
    }
    return -1;
}

/* The whole call is in sym[]: EOS at character e */
static void finish(dsc_rx_t *rx, int e, int eos) {
    int        c[DSC_MAX_CHARS + 2];
    int        bad = 0, ecc = 0;
    dsc_call_t call;

    for (int j = 0; j <= e + 1; j++) {
        c[j] = char_at(rx, j);
        bad += c[j] < 0;
    }
    if (bad > MAX_BAD)
        return;

    /* ECC: parity of one format specifier, the message and one EOS */
    ecc = c[0] >= 0 ? c[0] : c[1];
    for (int j = 2; j < e; j++)
        ecc ^= c[j];
    ecc ^= eos;

    memset(&call, 0, sizeof(call));
    call.fmt    = c[0] >= 0 ? c[0] : c[1];
    call.cat    = call.tc1 = call.tc2 = call.nature = call.subseq = -1;
    call.eos    = eos;
    call.ecc_ok = !bad && c[0] == c[1] && ecc == c[e + 1];
    if (call.fmt != 102 && call.fmt != 112 && call.fmt != 114 && call.fmt != 116 && call.fmt != 120 &&
        call.fmt != 123)
        return;

    const int *m = c + 2, *end = c + e;

#define NEED(n) if (end - m < (n)) goto done
    if (call.fmt != 112 && call.fmt != 116) {
        NEED(5);
        if (call.fmt == 102)
            area(m, call.to, sizeof(call.to));
        else
            mmsi(m, call.to);
        m += 5;
    }
    if (call.fmt != 112) {
        NEED(1);
        call.cat = *m++;
    }
    NEED(5);
    mmsi(m, call.from);
    m += 5;
    if (call.fmt != 112) {
        NEED(1);
        call.tc1 = *m++;
    }
    if (call.fmt == 112 || (call.cat == 112 && (call.tc1 == 110 || call.tc1 == 112))) {
        if (call.fmt == 112) {
            strcpy(call.dmmsi, call.from);
        } else {
            NEED(5);
            mmsi(m, call.dmmsi);
            m += 5;
        }
        NEED(9);
        call.nature = m[0];
        position(m + 1, call.pos, sizeof(call.pos));
        utc(m + 6, call.utc);
        call.subseq = m[8];
    } else {
        NEED(1);
        call.tc2 = *m++;
        if (end - m >= 3)
            frequency(m, call.freq, sizeof(call.freq));
    }
#undef NEED
done:
    if (call.from[0])
        rx->cb(&call, rx->userdata);
}

void dsc_rx_init(dsc_rx_t *rx, dsc_call_cb_t cb, void *userdata) {
    memset(rx, 0, sizeof(*rx));
    rx->cb       = cb;
    rx->userdata = userdata;
}

void dsc_rx_bit(dsc_rx_t *rx, int y) {
    rx->bits[(unsigned)rx->nbits % sizeof(rx->bits)] = y != 0;
    rx->nbits++;
    if (rx->nbits >= 1000000)   /* keeps the ring index in step (1000000 % 80 == 0) */
        rx->nbits -= 1000000 - 80;

    if (!rx->locked) {
        /* Phasing (M.493 § 3.3): 3 phasing characters in their places, at least one RX */
        int s[8], have = rx->nbits / 10 < 8 ? rx->nbits / 10 : 8;

        if (have < 3)
            return;
        for (int k = 0; k < have; k++)
            s[k] = symbol(rx, k);
        for (int q = 2; q <= 15; q++) {
            int ok = 0, rxs = 0;
            for (int k = 0; k < have && k <= q; k++) {
                if (s[k] >= 0 && s[k] == phasing(q - k)) {
                    ok++;
                    rxs += (q - k) % 2;
                }
            }
            if (ok >= 3 && rxs) {
                for (int p = 0; p < NPOS; p++)
                    rx->sym[p] = -1;
                for (int k = 0; k < have && k <= q; k++)
                    rx->sym[q - k] = s[k];
                rx->locked = true;
                rx->pos    = q;
                rx->bitcnt = 0;
                return;
            }
        }
        return;
    }

    if (++rx->bitcnt < 10)
        return;
    rx->bitcnt = 0;
    rx->pos++;
    rx->sym[rx->pos] = symbol(rx, 0);

    /* The RX copy of the ECC is in: EOS at character e ends the call */
    int e = (rx->pos - POS_FMT - RX_DELAY - 2) / 2;
    if ((rx->pos - POS_FMT - RX_DELAY) % 2 == 0 && e >= 12) {
        int eos = eos_at(rx, e);
        if (eos >= 0) {
            finish(rx, e, eos);
            rx->locked = false;
            rx->nbits  = 0;
            return;
        }
    }
    if (rx->pos + 1 >= NPOS)
        rx->locked = false;   /* no end: not a call */
}

const char *dsc_cat_name(const dsc_call_t *c) {
    if (c->fmt == 112 || (c->cat == 112 && c->tc1 == 112))
        return "distress";
    if (c->cat == 112)
        return c->tc1 == 110 ? "ack" : "distress";
    if (c->cat == 110)
        return "urgency";
    if (c->cat == 108)
        return "safety";
    return "routine";
}

const char *dsc_nature_name(int code) {
    static const char *names[] = {"FIRE / EXPLOSION", "FLOODING",       "COLLISION",        "GROUNDING",
                                  "LISTING / CAPSIZING", "SINKING",     "DISABLED / ADRIFT", "UNDESIGNATED",
                                  "ABANDONING SHIP", "PIRACY / ATTACK", "MAN OVERBOARD",    "",
                                  "EPIRB"};

    return code >= 100 && code <= 112 ? names[code - 100] : "";
}

const char *dsc_tc_name(int code) {
    switch (code) {
        case 100: return "F3E/G3E tel.";
        case 101: return "F3E/G3E duplex tel.";
        case 103: return "polling";
        case 104: return "unable to comply";
        case 105: return "end of call";
        case 106: return "data";
        case 109: return "J3E tel.";
        case 110: return "distress ack";
        case 112: return "distress relay";
        case 113: return "FEC";
        case 115: return "ARQ";
        case 118: return "test call";
        case 121: return "position";
        default: return "";
    }
}

/* ITU Maritime Identification Digits; the prefix finds the flag (flags.h) */
static const struct {
    short       mid;
    const char *prefix, *name;
} mids[] = {
    {201, "ZA", "Albania"}, {202, "C3", "Andorra"}, {203, "OE", "Austria"}, {204, "CU", "Azores"},
    {205, "ON", "Belgium"}, {206, "EU", "Belarus"}, {207, "LZ", "Bulgaria"}, {208, "HV", "Vatican"},
    {209, "5B", "Cyprus"}, {210, "5B", "Cyprus"}, {211, "DL", "Germany"}, {212, "5B", "Cyprus"},
    {213, "4L", "Georgia"}, {214, "ER", "Moldova"}, {215, "9H", "Malta"}, {216, "EK", "Armenia"},
    {218, "DL", "Germany"}, {219, "OZ", "Denmark"}, {220, "OZ", "Denmark"}, {224, "EA", "Spain"},
    {225, "EA", "Spain"}, {226, "F", "France"}, {227, "F", "France"}, {228, "F", "France"},
    {229, "9H", "Malta"}, {230, "OH", "Finland"}, {231, "OY", "Faroe Is."}, {232, "G", "United Kingdom"},
    {233, "G", "United Kingdom"}, {234, "G", "United Kingdom"}, {235, "G", "United Kingdom"},
    {236, "ZB", "Gibraltar"}, {237, "SV", "Greece"}, {238, "9A", "Croatia"}, {239, "SV", "Greece"},
    {240, "SV", "Greece"}, {241, "SV", "Greece"}, {242, "CN", "Morocco"}, {243, "HA", "Hungary"},
    {244, "PA", "Netherlands"}, {245, "PA", "Netherlands"}, {246, "PA", "Netherlands"}, {247, "I", "Italy"},
    {248, "9H", "Malta"}, {249, "9H", "Malta"}, {250, "EI", "Ireland"}, {251, "TF", "Iceland"},
    {252, "HB0", "Liechtenstein"}, {253, "LX", "Luxembourg"}, {254, "3A", "Monaco"}, {255, "CT3", "Madeira"},
    {256, "9H", "Malta"}, {257, "LA", "Norway"}, {258, "LA", "Norway"}, {259, "LA", "Norway"},
    {261, "SP", "Poland"}, {262, "4O", "Montenegro"}, {263, "CT", "Portugal"}, {264, "YO", "Romania"},
    {265, "SM", "Sweden"}, {266, "SM", "Sweden"}, {267, "OM", "Slovakia"}, {268, "T7", "San Marino"},
    {269, "HB", "Switzerland"}, {270, "OK", "Czech Rep."}, {271, "TA", "Turkey"}, {272, "UR", "Ukraine"},
    {273, "UA", "Russia"}, {274, "Z3", "North Macedonia"}, {275, "YL", "Latvia"}, {276, "ES", "Estonia"},
    {277, "LY", "Lithuania"}, {278, "S5", "Slovenia"}, {279, "YU", "Serbia"},
    {301, "VP2E", "Anguilla"}, {303, "K", "Alaska"}, {304, "V2", "Antigua"}, {305, "V2", "Antigua"},
    {306, "PJ2", "Curacao"}, {307, "P4", "Aruba"}, {308, "C6", "Bahamas"}, {309, "C6", "Bahamas"},
    {310, "VP9", "Bermuda"}, {311, "C6", "Bahamas"}, {312, "V3", "Belize"}, {314, "8P", "Barbados"},
    {316, "VE", "Canada"}, {319, "ZF", "Cayman Is."}, {321, "TI", "Costa Rica"}, {323, "CM", "Cuba"},
    {325, "J7", "Dominica"}, {327, "HI", "Dominican Rep."}, {329, "FG", "Guadeloupe"}, {330, "J3", "Grenada"},
    {331, "OX", "Greenland"}, {332, "TG", "Guatemala"}, {334, "HR", "Honduras"}, {336, "HH", "Haiti"},
    {338, "K", "USA"}, {339, "6Y", "Jamaica"}, {341, "V4", "St Kitts & Nevis"}, {343, "J6", "St Lucia"},
    {345, "XE", "Mexico"}, {347, "FM", "Martinique"}, {348, "VP2M", "Montserrat"}, {350, "YN", "Nicaragua"},
    {351, "HP", "Panama"}, {352, "HP", "Panama"}, {353, "HP", "Panama"}, {354, "HP", "Panama"},
    {355, "HP", "Panama"}, {356, "HP", "Panama"}, {357, "HP", "Panama"}, {358, "KP4", "Puerto Rico"},
    {359, "YS", "El Salvador"}, {361, "FP", "St Pierre"}, {362, "9Y", "Trinidad & Tobago"},
    {364, "VP5", "Turks & Caicos"}, {366, "K", "USA"}, {367, "K", "USA"}, {368, "K", "USA"}, {369, "K", "USA"},
    {370, "HP", "Panama"}, {371, "HP", "Panama"}, {372, "HP", "Panama"}, {373, "HP", "Panama"},
    {374, "HP", "Panama"}, {375, "J8", "St Vincent"}, {376, "J8", "St Vincent"}, {377, "J8", "St Vincent"},
    {378, "VP2V", "British Virgin Is."}, {379, "KP2", "US Virgin Is."},
    {401, "YA", "Afghanistan"}, {403, "HZ", "Saudi Arabia"}, {405, "S2", "Bangladesh"}, {408, "A9", "Bahrain"},
    {410, "A5", "Bhutan"}, {412, "BY", "China"}, {413, "BY", "China"}, {414, "BY", "China"},
    {416, "BV", "Taiwan"}, {417, "4S", "Sri Lanka"}, {419, "VU", "India"}, {422, "EP", "Iran"},
    {423, "4J", "Azerbaijan"}, {425, "YI", "Iraq"}, {428, "4X", "Israel"}, {431, "JA", "Japan"},
    {432, "JA", "Japan"}, {434, "EZ", "Turkmenistan"}, {436, "UN", "Kazakhstan"}, {437, "UK", "Uzbekistan"},
    {438, "JY", "Jordan"}, {440, "HL", "Korea"}, {441, "HL", "Korea"}, {443, "E4", "Palestine"},
    {445, "P5", "North Korea"}, {447, "9K", "Kuwait"}, {450, "OD", "Lebanon"}, {451, "EX", "Kyrgyzstan"},
    {453, "XX9", "Macao"}, {455, "8Q", "Maldives"}, {457, "JT", "Mongolia"}, {459, "9N", "Nepal"},
    {461, "A4", "Oman"}, {463, "AP", "Pakistan"}, {466, "A7", "Qatar"}, {468, "YK", "Syria"},
    {470, "A6", "UAE"}, {471, "A6", "UAE"}, {472, "EY", "Tajikistan"}, {473, "7O", "Yemen"},
    {475, "7O", "Yemen"}, {477, "VR", "Hong Kong"}, {478, "E7", "Bosnia & Herz."},
    {501, "F", "Adelie Land"}, {503, "VK", "Australia"}, {506, "XZ", "Myanmar"}, {508, "V8", "Brunei"},
    {510, "V6", "Micronesia"}, {511, "T8", "Palau"}, {512, "ZL", "New Zealand"}, {514, "XU", "Cambodia"},
    {515, "XU", "Cambodia"}, {516, "VK9X", "Christmas I."}, {518, "E5/s", "Cook Is."}, {520, "3D2", "Fiji"},
    {523, "VK9C", "Cocos Is."}, {525, "YB", "Indonesia"}, {529, "T30", "Kiribati"}, {531, "XW", "Laos"},
    {533, "9M2", "Malaysia"}, {536, "KH0", "N. Mariana Is."}, {538, "V7", "Marshall Is."},
    {540, "FK", "New Caledonia"}, {542, "E6", "Niue"}, {544, "C2", "Nauru"}, {546, "FO", "French Polynesia"},
    {548, "DU", "Philippines"}, {550, "4W", "Timor-Leste"}, {553, "P2", "Papua New Guinea"},
    {555, "VP6", "Pitcairn"}, {557, "H4", "Solomon Is."}, {559, "KH8", "American Samoa"}, {561, "5W", "Samoa"},
    {563, "9V", "Singapore"}, {564, "9V", "Singapore"}, {565, "9V", "Singapore"}, {566, "9V", "Singapore"},
    {567, "HS", "Thailand"}, {570, "A3", "Tonga"}, {572, "T2", "Tuvalu"}, {574, "3W", "Vietnam"},
    {576, "YJ", "Vanuatu"}, {577, "YJ", "Vanuatu"}, {578, "FW", "Wallis & Futuna"},
    {601, "ZS", "South Africa"}, {603, "D2", "Angola"}, {605, "7X", "Algeria"}, {607, "F", "St Paul & Amsterdam"},
    {608, "ZD8", "Ascension"}, {609, "9U", "Burundi"}, {610, "TY", "Benin"}, {611, "A2", "Botswana"},
    {612, "TL", "Central African Rep."}, {613, "TJ", "Cameroon"}, {615, "TN", "Congo"}, {616, "D6", "Comoros"},
    {617, "D4", "Cabo Verde"}, {618, "F", "Crozet"}, {619, "TU", "Ivory Coast"}, {620, "D6", "Comoros"},
    {621, "J2", "Djibouti"}, {622, "SU", "Egypt"}, {624, "ET", "Ethiopia"}, {625, "E3", "Eritrea"},
    {626, "TR", "Gabon"}, {627, "9G", "Ghana"}, {629, "C5", "Gambia"}, {630, "J5", "Guinea-Bissau"},
    {631, "3C", "Equatorial Guinea"}, {632, "3X", "Guinea"}, {633, "XT", "Burkina Faso"}, {634, "5Z", "Kenya"},
    {635, "F", "Kerguelen"}, {636, "EL", "Liberia"}, {637, "EL", "Liberia"}, {638, "Z8", "South Sudan"},
    {642, "5A", "Libya"}, {644, "7P", "Lesotho"}, {645, "3B8", "Mauritius"}, {647, "5R", "Madagascar"},
    {649, "TZ", "Mali"}, {650, "C9", "Mozambique"}, {654, "5T", "Mauritania"}, {655, "7Q", "Malawi"},
    {656, "5U", "Niger"}, {657, "5N", "Nigeria"}, {659, "V5", "Namibia"}, {660, "FR", "Reunion"},
    {661, "9X", "Rwanda"}, {662, "ST", "Sudan"}, {663, "6W", "Senegal"}, {664, "S7", "Seychelles"},
    {665, "ZD7", "St Helena"}, {666, "T5", "Somalia"}, {667, "9L", "Sierra Leone"}, {668, "S9", "Sao Tome"},
    {669, "3DA", "Eswatini"}, {670, "TT", "Chad"}, {671, "5V", "Togo"}, {672, "3V", "Tunisia"},
    {674, "5H", "Tanzania"}, {675, "5X", "Uganda"}, {676, "9Q", "DR Congo"}, {677, "5H", "Tanzania"},
    {678, "9J", "Zambia"}, {679, "Z2", "Zimbabwe"},
    {701, "LU", "Argentina"}, {710, "PY", "Brazil"}, {720, "CP", "Bolivia"}, {725, "CE", "Chile"},
    {730, "HK", "Colombia"}, {735, "HC", "Ecuador"}, {740, "VP8", "Falkland Is."}, {745, "FY", "French Guiana"},
    {750, "8R", "Guyana"}, {755, "ZP", "Paraguay"}, {760, "OA", "Peru"}, {765, "PZ", "Suriname"},
    {770, "CX", "Uruguay"}, {775, "YV", "Venezuela"},
};

const char *dsc_country(const char *mmsi, const char **prefix) {
    const char *m = mmsi;

    if (m[0] == '0')                    /* group 0MID..., coast station 00MID... */
        m += m[1] == '0' ? 2 : 1;
    else if (!strncmp(m, "111", 3))     /* SAR aircraft 111MID... */
        m += 3;
    else if (m[0] == '9' && (m[1] == '7' || m[1] == '8' || m[1] == '9'))   /* AtoN, craft with a mother ship */
        m += 2;
    int mid = (m[0] - '0') * 100 + (m[1] - '0') * 10 + (m[2] - '0');
    for (size_t i = 0; i < sizeof(mids) / sizeof(mids[0]); i++)
        if (mids[i].mid == mid) {
            if (prefix)
                *prefix = mids[i].prefix;
            return mids[i].name;
        }
    return NULL;
}
