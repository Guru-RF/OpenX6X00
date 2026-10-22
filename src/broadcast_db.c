#include "board.h"
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Xiegu X6100 LVGL GUI - broadcast stations from the EiBi CSV (broadcast_db.h)
//
// Copyright (c) 2026 Giovanni Ricciardi IU1WXY, own code of the IU1WXY firmware

#define _DEFAULT_SOURCE
#include "broadcast_db.h"     /* broadcast_match_t */

#include <ctype.h>      /* isdigit */
#include <math.h>       /* fabs */
#include <stdio.h>      /* fopen, fgets, snprintf */
#include <stdlib.h>     /* realloc, strtod */
#include <string.h>     /* strsep, strspn, memset */
#include <time.h>       /* gmtime_r */
#include <unistd.h>     /* access, unlink */

#define DB_FILE     BOARD_DATA "/broadcast.csv"
#define DB_DEFAULT  "/usr/share/x6100/broadcast.csv"

/* The EiBi columns: kHz;Time(UTC);Days;ITU;Station;Lng;Target;Remarks;P;Start;Stop */
enum { C_KHZ, C_TIME, C_DAYS, C_ITU, C_STATION, C_LANG, C_TARGET, C_REMARKS, C_P, C_START, C_STOP, COLS };

typedef struct station {
    broadcast_match_t   m;
    uint8_t             wdays;      /* bit 0 Sunday .. bit 6 Saturday */
    int                 from_md;    /* season month * 100 + day, -1 none */
    int                 to_md;
} station_t;

static station_t    *list;
static size_t       list_len;
static size_t       list_cap;

static bool copy_default(void) {
    FILE    *src = fopen(DB_DEFAULT, "rb");
    FILE    *dst = src ? fopen(DB_FILE, "wb") : NULL;
    char    chunk[4096];
    size_t  got;
    bool    copied = dst != NULL;

    while (copied && (got = fread(chunk, 1, sizeof(chunk), src)) > 0)
        copied = fwrite(chunk, 1, got, dst) == got;
    if (src) {
        copied = copied && !ferror(src);
        fclose(src);
    }
    if (dst) {
        copied = fclose(dst) == 0 && copied;
        if (!copied) unlink(DB_FILE);     /* no half file: the next start tries again */
    }
    return copied;
}

/* "HHMM" to minutes of the day, 2400 is the end of it; -1 if not a time */
static int minutes(const char *s) {
    for (int i = 0; i < 4; i++)
        if (!isdigit((unsigned char)s[i])) return -1;

    int h = (s[0] - '0') * 10 + s[1] - '0';
    int m = (s[2] - '0') * 10 + s[3] - '0';

    if (h == 24 && m == 0) return 24 * 60;
    return (h < 24 && m < 60) ? h * 60 + m : -1;
}

/* Text field: cut to the size, no line end at the tail */
static void field(char *dst, size_t size, const char *src) {
    size_t n;

    snprintf(dst, size, "%s", src);
    n = strlen(dst);
    while (n && (dst[n - 1] == '\n' || dst[n - 1] == '\r')) dst[--n] = 0;
}

/* The station name in plain ASCII for the radio's fonts. EiBi files often went through a Mac
 * and back, so some Latin-1 letters stand for another accented one (U+00D2 is an n with tilde): those are
 * mapped to what they meant. A byte that does not start a UTF-8 character, or anything without
 * a letter here, is a '?'. */
static const char latin1_upper[] =
    "AAAAAAAC" "eEEEieII" "DNnOOOO?" "OoUoUYTs"     /* U+00C0 .. U+00DF */
    "aaaaaaac" "eeeeiiii" "dnooooo?" "ouuuuy?y";    /* U+00E0 .. U+00FF */

static char ascii_of(uint32_t code) {
    if (code >= 0x20 && code < 0x7F) return code;
    if (code >= 0xC0 && code <= 0xFF) return latin1_upper[code - 0xC0];

    switch (code) {
        case 0xAC:  return 'A';
        case 0xAF:  return 'o';
        case 0xB7:  return 'a';
        case 0x2D9: return 'o';
        default:    return '?';     /* no plain letter for it */
    }
}

static void station_name(char *dst, size_t size, const char *src) {
    const unsigned char *p = (const unsigned char *)src;      /* bytes, unsigned for the UTF-8 tests */
    size_t              n = 0;

    while (*p && n + 1 < size) {
        uint32_t    code;
        int         len;

        if (*p < 0x80)                  { code = *p;          len = 1; }
        else if ((*p & 0xE0) == 0xC0)   { code = *p & 0x1F;   len = 2; }
        else if ((*p & 0xF0) == 0xE0)   { code = *p & 0x0F;   len = 3; }
        else if ((*p & 0xF8) == 0xF0)   { code = *p & 0x07;   len = 4; }
        else                            { code = '?';         len = 1; }

        /* the tail bytes are taken as they come, the string must just not end inside */
        for (int i = 1; i < len; i++) {
            if (!p[i]) {
                code = '?';
                len = 1;
                break;
            }
            code = (code << 6) | (p[i] & 0x3F);
        }
        dst[n++] = ascii_of(code);
        p += len;
    }
    dst[n] = 0;
}

static int wday_at(const char *s) {
    static const char names[] = "SuMoTuWeThFrSa";

    for (int d = 0; d < 7; d++)
        if (s[0] == names[d * 2] && s[0] && s[1] == names[d * 2 + 1]) return d;
    return -1;
}

/* The weekdays of an EiBi Days field: empty, "irr" and "Test" are every day; digits are
 * 1 Monday .. 7 Sunday; otherwise day names, alone ("SaSu", "Mo,We") or as ranges ("Fr-Tu"). */
static uint8_t wdays_of(const char *s) {
    uint8_t mask = 0;

    if (!s[0] || !strcmp(s, "irr") || !strcmp(s, "Test")) return 0x7F;
    if (!s[strspn(s, "0123456789,")]) {
        for (int d = 1; d <= 7; d++)
            if (strchr(s, '0' + d)) mask |= 1 << (d % 7);
        return mask;
    }
    while (*s) {
        int a = wday_at(s), b;

        if (a < 0) {
            s++;
            continue;
        }
        s += 2;
        b = *s == '-' ? wday_at(s + 1) : -1;
        if (b < 0) {
            if (*s == '-') s++;
            mask |= 1 << a;
            continue;
        }
        for (int d = a; ; d = (d + 1) % 7) {
            mask |= 1 << d;
            if (d == b) break;
        }
        s += 3;
    }
    return mask;
}

/* "DDMM" season limit to month * 100 + day; "[...]" is only a note, -1 */
static int month_day(const char *s) {
    for (int i = 0; i < 4; i++)
        if (!isdigit((unsigned char)s[i])) return -1;

    int d = (s[0] - '0') * 10 + s[1] - '0';
    int m = (s[2] - '0') * 10 + s[3] - '0';

    return (d >= 1 && d <= 31 && m >= 1 && m <= 12) ? m * 100 + d : -1;
}

static bool parse_line(char *line, station_t *st) {
    char        *col[COLS];
    const char  *t;
    size_t      n = 0;

    while (n < COLS && line) col[n++] = strsep(&line, ";");
    if (n <= C_STATION) return false;
    for (size_t i = n; i < COLS; i++) col[i] = "";

    memset(st, 0, sizeof(*st));
    st->m.freq_khz = strtod(col[C_KHZ], NULL);
    t = col[C_TIME];
    if (st->m.freq_khz <= 0 || strlen(t) < 9 || t[4] != '-') return false;
    st->m.start_minute = minutes(t);
    st->m.stop_minute = minutes(t + 5);
    if (st->m.start_minute < 0 || st->m.stop_minute < 0) return false;

    station_name(st->m.station, sizeof(st->m.station), col[C_STATION]);
    field(st->m.days, sizeof(st->m.days), col[C_DAYS]);
    field(st->m.itu, sizeof(st->m.itu), col[C_ITU]);
    field(st->m.language, sizeof(st->m.language), col[C_LANG]);
    field(st->m.target, sizeof(st->m.target), col[C_TARGET]);
    field(st->m.remarks, sizeof(st->m.remarks), col[C_REMARKS]);
    st->wdays = wdays_of(st->m.days);
    st->from_md = month_day(col[C_START]);
    st->to_md = month_day(col[C_STOP]);
    return st->m.station[0] != 0;        /* nothing left of the name: not a station */
}

bool broadcast_db_init(void) {
    FILE        *f;
    char        line[1024];     /* a longer line goes on as the next one, as fgets() gives it */
    station_t   st;

    free(list);
    list = NULL;
    list_len = list_cap = 0;

    if (access(DB_FILE, F_OK) != 0 && !copy_default()) return false;
    f = fopen(DB_FILE, "r");
    if (!f) return false;

    fgets(line, sizeof(line), f);   /* the column names */
    while (fgets(line, sizeof(line), f)) {
        if (!parse_line(line, &st)) continue;
        if (list_len == list_cap) {
            size_t      cap = list_cap ? list_cap * 2 : 1024;
            station_t   *p = realloc(list, cap * sizeof(*list));

            if (!p) {
                fclose(f);
                return false;
            }
            list = p;
            list_cap = cap;
        }
        list[list_len++] = st;
    }
    fclose(f);
    return list_len > 0;
}

static bool on_air(const station_t *st, const struct tm *utc) {
    int now = utc->tm_hour * 60 + utc->tm_min;
    int from = st->m.start_minute, to = st->m.stop_minute;
    int wday = utc->tm_wday;
    int md = (utc->tm_mon + 1) * 100 + utc->tm_mday;

    /* from == to is all day; past midnight the hours still belong to the day it started */
    if (from < to && (now < from || now >= to)) return false;
    if (from > to) {
        if (now < from && now >= to) return false;
        if (now < to) wday = (wday + 6) % 7;
    }
    if (!(st->wdays & (1 << wday))) return false;

    /* a season that wraps the new year (Oct to Mar) is the outside of the range */
    if (st->from_md >= 0 && st->to_md >= 0) {
        if (st->from_md <= st->to_md) return md >= st->from_md && md <= st->to_md;
        return md >= st->from_md || md <= st->to_md;
    }
    if (st->from_md >= 0) return md >= st->from_md;
    if (st->to_md >= 0) return md <= st->to_md;
    return true;
}

size_t broadcast_db_find_matches(int32_t freq_hz, int tolerance_hz, broadcast_match_t *out, size_t max) {
    time_t      now = time(NULL);
    struct tm   utc;
    size_t      n = 0;

    if (!list || !out || !max || !gmtime_r(&now, &utc)) return 0;

    for (size_t i = 0; i < list_len && n < max; i++) {
        if (fabs(list[i].m.freq_khz * 1000.0 - freq_hz) > tolerance_hz) continue;
        if (on_air(&list[i], &utc)) out[n++] = list[i].m;
    }
    return n;
}
