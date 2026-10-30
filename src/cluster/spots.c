/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  DX cluster spots: line parser, spot store, band and mode helpers.
 */

#define _GNU_SOURCE /* strcasestr */

#include "spots.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Above 6 m the bands a transverter or the X6200's WR12 can reach, as wide as any region has them (issue #103):
 * 4 m where Region 1 countries have it, 2 m and 70 cm as the Americas do. 23 cm is out of reach (a transverter
 * goes up to 500 MHz, the WR12 to 1 GHz). Lowest first, as Settings > Cluster lists them; cluster.conf keeps the
 * bands by name, not by index */
static const struct {
    const char *name;
    int32_t     low, high;
} bands[] = {
    {"160m", 1800000, 2000000},     {"80m", 3500000, 3800000},     {"60m", 5351500, 5366500},
    {"40m", 7000000, 7200000},      {"30m", 10100000, 10150000},   {"20m", 14000000, 14350000},
    {"17m", 18068000, 18168000},    {"15m", 21000000, 21450000},   {"12m", 24890000, 24990000},
    {"10m", 28000000, 29700000},    {"8m", 40000000, 42000000},    {"6m", 50000000, 52000000},
    {"4m", 69900000, 70500000},     {"2m", 144000000, 148000000},  {"70cm", 420000000, 450000000},
};

#define BANDS_COUNT ((int)(sizeof(bands) / sizeof(bands[0])))

/* A band is a bit of the filters' uint16_t (cluster_filter_t, cluster_alert_t) */
_Static_assert(BANDS_COUNT <= 16, "more bands than bits in the cluster filters");

int cluster_band_of(int32_t hz) {
    for (int i = 0; i < BANDS_COUNT; i++)
        if (hz >= bands[i].low && hz <= bands[i].high) return i;
    return -1;
}

const char *cluster_band_name(int band) {
    return (band >= 0 && band < BANDS_COUNT) ? bands[band].name : "";
}

static void copy_word(char *dst, size_t size, const char *src, size_t n) {
    if (n >= size) n = size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static const char *skip_space(const char *p) {
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

static const char *word_end(const char *p) {
    while (*p && !isspace((unsigned char)*p)) p++;
    return p;
}

/* "1238Z" -> minutes after midnight, or -1 */
static int parse_hhmmz(const char *p, size_t n) {
    if (n != 5 || toupper((unsigned char)p[4]) != 'Z') return -1;
    for (int i = 0; i < 4; i++)
        if (!isdigit((unsigned char)p[i])) return -1;
    int h = (p[0] - '0') * 10 + (p[1] - '0');
    int m = (p[2] - '0') * 10 + (p[3] - '0');
    if (h > 23 || m > 59) return -1;
    return h * 60 + m;
}

static void set_comment(cluster_spot_t *out, const char *b, const char *e) {
    b = skip_space(b);
    while (e > b && isspace((unsigned char)e[-1])) e--;
    copy_word(out->comment, sizeof(out->comment), b, (size_t)(e - b));
}

static bool parse_freq_call(const char **pp, cluster_spot_t *out) {
    const char *p = skip_space(*pp);
    char       *end;
    double      khz = strtod(p, &end);
    if (end == p || khz < 100.0 || khz > 1.0e7) return false;
    p = skip_space(end);
    const char *e = word_end(p);
    bool digit = false, alpha = false;
    for (const char *c = p; c < e; c++) {
        digit |= isdigit((unsigned char)*c) != 0;
        alpha |= isalpha((unsigned char)*c) != 0;
    }
    if (!digit || !alpha) return false; /* not a callsign */
    copy_word(out->dx, sizeof(out->dx), p, (size_t)(e - p));
    out->freq_hz = (int32_t)(khz * 1000.0 + 0.5);
    *pp = e;
    return true;
}

/* DX de DL1ABC:     14023.5  VP8LP        CW 599 up 2          1238Z JO62 */
static bool parse_dx_de(const char *p, cluster_spot_t *out) {
    p += 5;
    p = skip_space(p);
    const char *colon = strchr(p, ':');
    if (!colon) return false;
    const char *e = p;
    while (e < colon && !isspace((unsigned char)*e)) e++;
    copy_word(out->spotter, sizeof(out->spotter), p, (size_t)(e - p));
    p = colon + 1;
    if (!parse_freq_call(&p, out)) return false;

    /* The time is the last HHMMZ word; the comment is what comes before it. */
    const char *time_at = NULL;
    int         utc = -1;
    for (const char *w = skip_space(p); *w; w = skip_space(word_end(w))) {
        int t = parse_hhmmz(w, (size_t)(word_end(w) - w));
        if (t >= 0) {
            time_at = w;
            utc     = t;
        }
    }
    if (!time_at) return false;
    out->utc_min = (uint16_t)utc;
    set_comment(out, p, time_at);
    return true;
}

/* "30-Sep-2026" or "30-Sep-26" -> days since 1970-01-01, or -1 */
static int64_t parse_date(const char *p, size_t n) {
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4];
    int  d, y, used = 0;
    if (sscanf(p, "%2d-%3[A-Za-z]-%4d%n", &d, mon, &y, &used) != 3 || (size_t)used != n) return -1;
    const char *m = strcasestr(months, mon);
    if (y < 100) y += 2000;
    if (!m || (m - months) % 3 || d < 1 || d > 31 || y < 2000) return -1;
    int mo = (int)(m - months) / 3 + 1;
    /* days from civil (H. Hinnant) */
    y -= mo <= 2;
    int64_t era = y / 400, yoe = y - era * 400;
    int64_t doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
}

/* sh/dx row (DXSpider, CC Cluster, AR-Cluster):
 *   14023.5  VP8LP       28-Sep-2026 1238Z  CW 599 up 2          <DL1ABC> */
static bool parse_show_dx(const char *p, cluster_spot_t *out) {
    if (!parse_freq_call(&p, out)) return false;
    p = skip_space(p);
    const char *e    = word_end(p);
    int64_t     days = parse_date(p, (size_t)(e - p));
    if (days < 0 && (e - p < 5 || !memchr(p, '-', (size_t)(e - p)))) return false; /* other date: no age */
    p = skip_space(e);
    e = word_end(p);
    int utc = parse_hhmmz(p, (size_t)(e - p));
    if (utc < 0) return false;
    out->utc_min = (uint16_t)utc;
    out->when    = days < 0 ? 0 : days * 86400 + utc * 60;

    const char *lt = strrchr(e, '<');
    const char *gt = lt ? strchr(lt, '>') : NULL;
    if (lt && gt) {
        copy_word(out->spotter, sizeof(out->spotter), lt + 1, (size_t)(gt - lt - 1));
        set_comment(out, e, lt);
    } else {
        set_comment(out, e, e + strlen(e));
    }
    return true;
}

bool cluster_parse_line(const char *line, cluster_spot_t *out) {
    memset(out, 0, sizeof(*out));
    const char *p = skip_space(line);
    if (strncasecmp(p, "DX de ", 6) == 0) return parse_dx_de(p, out);
    if (isdigit((unsigned char)*p)) return parse_show_dx(p, out);
    return false;
}

/* Spot time a is after b (within 12 hours, across midnight). */
static bool newer(uint16_t a, uint16_t b) {
    return a != b && (a + 1440 - b) % 1440 < 720;
}

void cluster_store_add(cluster_store_t *s, const cluster_spot_t *spot) {
    for (int i = 0; i < s->count; i++) {
        if (strcasecmp(s->spots[i].dx, spot->dx) == 0 && abs(s->spots[i].freq_hz - spot->freq_hz) <= 1000) {
            if (newer(s->spots[i].utc_min, spot->utc_min)) return;
            memmove(&s->spots[i], &s->spots[i + 1], (size_t)(s->count - i - 1) * sizeof(s->spots[0]));
            s->count--;
            break;
        }
    }
    if (s->count == CLUSTER_MAX_SPOTS) {
        if (newer(s->spots[0].utc_min, spot->utc_min)) return;
        memmove(&s->spots[0], &s->spots[1], (size_t)(s->count - 1) * sizeof(s->spots[0]));
        s->count--;
    }
    /* sh/dx lists the newest first: insert by time, not at the end. */
    int i = s->count;
    while (i > 0 && newer(s->spots[i - 1].utc_min, spot->utc_min)) i--;
    memmove(&s->spots[i + 1], &s->spots[i], (size_t)(s->count - i) * sizeof(s->spots[0]));
    s->spots[i] = *spot;
    s->count++;
}

uint16_t cluster_age_min(uint16_t spot_utc_min, uint16_t now_utc_min) {
    return (uint16_t)((now_utc_min + 1440 - spot_utc_min) % 1440);
}

void cluster_store_expire(cluster_store_t *s, uint16_t now_utc_min, uint16_t max_age_min) {
    int n = 0;
    for (int i = 0; i < s->count; i++) {
        uint16_t age = cluster_age_min(s->spots[i].utc_min, now_utc_min);
        /* A few minutes "in the future" is clock skew, not a day-old spot. */
        if (age <= max_age_min || age >= 1440 - 5) s->spots[n++] = s->spots[i];
    }
    s->count = n;
}

static bool has_word(const char *text, const char *word) {
    size_t n = strlen(word);
    for (const char *p = text; *p; p++) {
        if (strncasecmp(p, word, n) != 0) continue;
        bool start = p == text || !isalnum((unsigned char)p[-1]);
        bool end   = !isalnum((unsigned char)p[n]);
        if (start && end) return true;
    }
    return false;
}

static const struct {
    int32_t low, high;
} cw_segments[] = {
    {1800000, 1838000},   {3500000, 3570000},   {7000000, 7040000},   {10100000, 10130000},
    {14000000, 14070000}, {18068000, 18095000}, {21000000, 21070000}, {24890000, 24915000},
    {28000000, 28070000}, {50000000, 50100000}, {69900000, 70100000}, {144000000, 144100000},
    {432000000, 432100000},
};

static const int32_t digi_khz[] = {
    1840,  3573,  3575,  5357,  7047,  7074,  10136, 10140, 14074, 14080,
    18100, 18104, 21074, 21140, 24915, 24919, 28074, 28180, 50313, 50318,
    70154, 144174,
};

/* FM above the narrow-band parts: 4 m from 70.250, 2 m from 144.500 (the channels, repeaters, APRS, the Americas'
 * 146-148), 70 cm outside 432.000-432.500 (Region 1's channels and repeaters 430-440, the Americas' 420-450) */
static const struct {
    int32_t low, high;
} fm_segments[] = {
    {29510000, 29700000}, {70250000, 70500000}, {144500000, 148000000}, {420000000, 432000000},
    {432500000, 450000000},
};

cluster_mode_t cluster_guess_mode(const cluster_spot_t *spot) {
    const char *c    = spot->comment;
    int32_t     hz   = spot->freq_hz;
    bool        high = hz >= 10000000;

    if (has_word(c, "CW")) return CLUSTER_MODE_CW;
    if (has_word(c, "FT8") || has_word(c, "FT4") || has_word(c, "RTTY") || has_word(c, "PSK") ||
        has_word(c, "PSK31") || has_word(c, "JS8") || has_word(c, "DIGI") || has_word(c, "DATA") ||
        has_word(c, "SSTV") || has_word(c, "OLIVIA"))
        return CLUSTER_MODE_DIGI;
    if (has_word(c, "LSB")) return CLUSTER_MODE_LSB;
    if (has_word(c, "USB")) return CLUSTER_MODE_USB;
    if (has_word(c, "SSB")) return high ? CLUSTER_MODE_USB : CLUSTER_MODE_LSB;
    if (has_word(c, "AM")) return CLUSTER_MODE_AM;
    if (has_word(c, "FM")) return CLUSTER_MODE_FM;

    for (size_t i = 0; i < sizeof(digi_khz) / sizeof(digi_khz[0]); i++)
        if (abs(hz - digi_khz[i] * 1000) <= 3000) return CLUSTER_MODE_DIGI;
    for (size_t i = 0; i < sizeof(cw_segments) / sizeof(cw_segments[0]); i++)
        if (hz >= cw_segments[i].low && hz < cw_segments[i].high) return CLUSTER_MODE_CW;
    if (hz >= 10100000 && hz <= 10150000) return CLUSTER_MODE_CW; /* 30m: no SSB */
    for (size_t i = 0; i < sizeof(fm_segments) / sizeof(fm_segments[0]); i++)
        if (hz >= fm_segments[i].low && hz <= fm_segments[i].high) return CLUSTER_MODE_FM;
    return high ? CLUSTER_MODE_USB : CLUSTER_MODE_LSB;
}
