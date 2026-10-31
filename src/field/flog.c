/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - Field Ops: the activation log
 */

#define _GNU_SOURCE

#include "flog.h"
#include "../board.h"
#include "../radio_types.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static const struct {
    int32_t     lo, hi;
    const char *name;
} bands[] = {
    {1800000, 2000000, "160m"},   {3500000, 4000000, "80m"},    {5060000, 5450000, "60m"},
    {7000000, 7300000, "40m"},    {10100000, 10150000, "30m"},  {14000000, 14350000, "20m"},
    {18068000, 18168000, "17m"},  {21000000, 21450000, "15m"},  {24890000, 24990000, "12m"},
    {28000000, 29700000, "10m"},  {50000000, 54000000, "6m"},   {144000000, 148000000, "2m"},
    {430000000, 440000000, "70cm"},
};

const char *field_band(int32_t hz) {
    for (size_t i = 0; i < sizeof(bands) / sizeof(bands[0]); i++)
        if (hz >= bands[i].lo && hz <= bands[i].hi)
            return bands[i].name;
    return "";
}

static size_t field(char *buf, size_t size, size_t len, const char *tag, const char *val) {
    if (!val[0] || len >= size)
        return len;
    int n = snprintf(buf + len, size - len, "<%s:%zu>%s ", tag, strlen(val), val);
    return n > 0 ? len + (size_t)n : len;
}

/* The log's modes that ADIF has as a SUBMODE, with their MODE (ADIF 3.1.8: JTTY came in 3.1.8, FT2 in 3.1.7).
 * FT8, RTTY, OLIVIA, SSTV, WSPR and the others stand on their own */
static const struct {
    const char *sub, *mode;
} submodes[] = {
    {"FT4", "MFSK"}, {"FT2", "MFSK"}, {"JS8", "MFSK"}, {"JTTY", "MFSK"}, {"PSK31", "PSK"}, {"PSK63", "PSK"},
};

/* The ADIF MODE of a log mode that is a SUBMODE, NULL when the mode stands on its own */
static const char *adif_mode_of(const char *sub) {
    for (size_t i = 0; i < sizeof(submodes) / sizeof(submodes[0]); i++)
        if (!strcasecmp(sub, submodes[i].sub))
            return submodes[i].mode;
    return NULL;
}

/* FT8 and FT4 dial frequencies, kHz: in USB, LSB or a data mode on one of them (500 Hz below to 3.5 kHz above)
 * the mode is that */
static const char *ftx_by_freq(int32_t hz) {
    static const int ft8[] = {1840, 3573, 5357, 7074, 10136, 14074, 18100, 21074, 24915, 28074, 50313};
    static const int ft4[] = {3575, 7047, 10140, 14080, 18104, 21140, 24919, 28180, 50318};

    for (size_t i = 0; i < sizeof(ft8) / sizeof(ft8[0]); i++)
        if (hz >= ft8[i] * 1000 - 500 && hz <= ft8[i] * 1000 + 3500)
            return "FT8";
    for (size_t i = 0; i < sizeof(ft4) / sizeof(ft4[0]); i++)
        if (hz >= ft4[i] * 1000 - 500 && hz <= ft4[i] * 1000 + 3500)
            return "FT4";
    return NULL;
}

const char *field_qso_mode(const char *app, int mode, int32_t hz) {
    /* The apps a QSO is made with, by their names on the frequency strip, and their log modes. WeFax and NavTex
     * receive only: the radio's mode, as with no app */
    static const char *const apps[][2] = {
        {"FT8", "FT8"},     {"FT4", "FT4"},       {"FT2", "FT2"},   {"JS8", "JS8"},   {"PSK31", "PSK31"},
        {"PSK63", "PSK63"}, {"Olivia", "OLIVIA"}, {"JTTY", "JTTY"}, {"RTTY", "RTTY"}, {"SSTV", "SSTV"},
        {"WSPR", "WSPR"},
    };
    const char *ftx = ftx_by_freq(hz);

    /* CW is CW, app or not: RTTY and SSTV leave the radio's mode free, and no app sends in CW */
    if (mode == radio_mode_cw || mode == radio_mode_cwr)
        return "CW";
    for (size_t i = 0; app && i < sizeof(apps) / sizeof(apps[0]); i++)
        if (!strcasecmp(app, apps[i][0]))
            return apps[i][1];
    switch (mode) {
        case radio_mode_am:
        case radio_mode_sam:
            return "AM";
        case radio_mode_nfm:
        case radio_mode_wfm:
            return "FM";
        case radio_mode_lsb_dig:
        case radio_mode_usb_dig:
            return ftx ? ftx : "";      /* PSK, Olivia, RTTY... from a program on TCI: not known, and not FT8 (#92) */
        default:
            return ftx ? ftx : "SSB";
    }
}

size_t field_adif_record(char *buf, size_t size, const field_qso_t *q, const field_station_t *st) {
    char      date[16], tm_on[16], freq[16];
    struct tm tm;
    size_t    n = 0;

    gmtime_r(&q->t, &tm);
    strftime(date, sizeof(date), "%Y%m%d", &tm);
    strftime(tm_on, sizeof(tm_on), "%H%M%S", &tm);
    snprintf(freq, sizeof(freq), "%.6f", q->freq_hz / 1e6);
    buf[0] = 0;
    n = field(buf, size, n, "CALL", q->call);
    n = field(buf, size, n, "QSO_DATE", date);
    n = field(buf, size, n, "TIME_ON", tm_on);
    n = field(buf, size, n, "BAND", field_band(q->freq_hz));
    n = field(buf, size, n, "FREQ", freq);
    /* ADIF has FT4, JS8, FT2 and JTTY as submodes of MFSK, PSK31 and PSK63 of PSK (FT8 is a mode of its own); a
     * mode not known goes as no MODE at all */
    const char *parent = adif_mode_of(q->mode);
    n = field(buf, size, n, "MODE", parent ? parent : q->mode);
    n = field(buf, size, n, "SUBMODE", parent ? q->mode : "");
    n = field(buf, size, n, "RST_SENT", q->rst_s);
    n = field(buf, size, n, "RST_RCVD", q->rst_r);
    n = field(buf, size, n, "NAME", q->name);
    n = field(buf, size, n, "QTH", q->qth);
    /* A tile they gave (tile to tile) is where they are, closer than a logbook's locator */
    char their = field_ref_prog(q->their_ref);
    n = field(buf, size, n, "GRIDSQUARE", their == 'G' ? q->their_ref : q->grid);
    n = field(buf, size, n, "COUNTRY", q->country);
    n = field(buf, size, n, "STATION_CALLSIGN", st->call);
    n = field(buf, size, n, "OPERATOR", st->call);
    /* Tiles: the tile is where the activation was, whatever the station's locator says; Tiles reads it here */
    n = field(buf, size, n, "MY_GRIDSQUARE", st->prog == 'G' ? st->ref : st->grid);
    /* TOTA, WCA, MOTA and Tiles have no ADIF field of their own: MY_SIG / MY_SIG_INFO, as their rules and GMA ask */
    if (st->prog == 'S') {
        n = field(buf, size, n, "MY_SOTA_REF", st->ref);
    } else if (field_prog_sig(st->prog)) {
        n = field(buf, size, n, "MY_SIG", field_prog_sig(st->prog));
        n = field(buf, size, n, "MY_SIG_INFO", st->ref);
        if (st->prog == 'P')
            n = field(buf, size, n, "MY_POTA_REF", st->ref);
    }
    /* The other station's reference: park to park, summit to summit... */
    if (their == 'S') {
        n = field(buf, size, n, "SOTA_REF", q->their_ref);
    } else if (field_prog_sig(their)) {
        n = field(buf, size, n, "SIG", field_prog_sig(their));
        n = field(buf, size, n, "SIG_INFO", q->their_ref);
        if (their == 'P')
            n = field(buf, size, n, "POTA_REF", q->their_ref);
    }
    if (n < size)
        n += (size_t)snprintf(buf + n, size - n, "<eor>\n");
    return n < size ? n : size - 1;
}

bool field_log_add(const char *path, const field_qso_t *q, const field_station_t *st) {
    char  rec[768], dir[256];
    int   mk = 0, err = 0;

    /* its folders first: a new or rewritten microSD has none */
    snprintf(dir, sizeof(dir), "%s", path);
    for (char *p = strchr(dir + 1, '/'); p; p = strchr(p + 1, '/')) {
        *p = 0;
        if (mkdir(dir, 0755) != 0 && errno != EEXIST)
            mk = errno;
        *p = '/';
    }

    /* The browser's log editor rewrites the file whole: it holds this lock on the folder while it
     * does, so a QSO is never appended to a copy about to be replaced (audit A11, apps.py edit_qso) */
    char *sl   = strrchr(dir, '/');
    int   lock = -1;
    if (sl && sl != dir) {
        *sl  = 0;
        lock = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (lock >= 0)
            flock(lock, LOCK_EX);
    }

    bool  is_new = access(path, F_OK) != 0;
    FILE *f      = fopen(path, "a");

    if (!f) {
        err = errno == ENOENT && mk ? mk : errno;   /* a folder that could not be made says why */
        fprintf(stderr, "Log write failed: %s: %s\n", path, strerror(err));
        if (lock >= 0)
            close(lock);
        errno = err;
        return false;
    }
    if (is_new)
        fprintf(f, BOARD_NAME " Field Ops log\n<ADIF_VER:5>3.1.8 <PROGRAMID:9>OpenX6X00 <eoh>\n");
    field_adif_record(rec, sizeof(rec), q, st);
    if (fputs(rec, f) < 0 || fflush(f) != 0)
        err = errno;
    if (!err && fsync(fileno(f)) != 0)   /* a QSO is never lost to a power cut; a card error is told */
        err = errno;
    if (fclose(f) != 0 && !err)
        err = errno;
    if (lock >= 0)
        close(lock);
    if (err)
        fprintf(stderr, "Log write failed: %s: %s\n", path, strerror(err));
    errno = err;
    return !err;
}

const char *field_log_error(int err) {
    switch (err) {
        case EROFS:
            return "the microSD has an error and is read-only. Restart the radio; if it happens again, "
                   "back up and rewrite the microSD";
        case ENOSPC:
        case EDQUOT:
            return "the microSD is full";
        case EIO:
            return "the microSD gave a write error. Restart the radio; if it happens again, "
                   "back up and rewrite the microSD";
        default:
            return "the microSD could not be written. Restart the radio; if it happens again, "
                   "send the diagnostics (Settings > Info)";
    }
}

/* The value of <tag:n> in the record, "" when missing */
static void get(const char *rec, const char *tag, char *out, size_t size) {
    size_t tl = strlen(tag);

    out[0] = 0;
    for (const char *p = strchr(rec, '<'); p; p = strchr(p + 1, '<')) {
        if (strncasecmp(p + 1, tag, tl) || p[1 + tl] != ':')
            continue;
        size_t len = strtoul(p + 2 + tl, NULL, 10);
        const char *v = strchr(p, '>');
        if (!v)
            return;
        v++;
        if (len > strlen(v))    /* the field runs past the record: a broken record, not a value (audit B06) */
            return;
        if (len >= size)
            len = size - 1;
        memcpy(out, v, len);
        out[len] = 0;
        return;
    }
}

static void reverse(field_qso_t *a, int lo, int hi) {
    while (lo < hi) {
        field_qso_t t = a[lo];
        a[lo++] = a[hi];
        a[hi--] = t;
    }
}

int field_log_read(const char *path, field_qso_t *out, int max) {
    FILE *f = fopen(path, "r");
    char  line[1024];
    int   n = 0;

    if (!f)
        return 0;
    /* field_log_add writes one record per line. Past max the oldest make room (a ring), so a
     * long activation shows its newest QSOs; the count returned is the whole file's */
    while (fgets(line, sizeof(line), f)) {
        char        v[32];
        struct tm   tm = {0};
        field_qso_t *q = &out[n % max];

        if (!strcasestr(line, "<eor>"))
            continue;
        get(line, "CALL", q->call, sizeof(q->call));
        if (!q->call[0])
            continue;
        get(line, "QSO_DATE", v, sizeof(v));
        sscanf(v, "%4d%2d%2d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday);
        get(line, "TIME_ON", v, sizeof(v));
        sscanf(v, "%2d%2d%2d", &tm.tm_hour, &tm.tm_min, &tm.tm_sec);
        tm.tm_year -= 1900;
        tm.tm_mon -= 1;
        q->t = timegm(&tm);
        get(line, "FREQ", v, sizeof(v));
        q->freq_hz = (int32_t)(atof(v) * 1e6 + 0.5);
        get(line, "MODE", q->mode, sizeof(q->mode));
        /* the submode names it: FT4, JS8, FT2, JTTY, PSK31, PSK63 */
        if (!strcasecmp(q->mode, "MFSK") || !strcasecmp(q->mode, "PSK")) {
            char sub[8];
            get(line, "SUBMODE", sub, sizeof(sub));
            if (sub[0])
                snprintf(q->mode, sizeof(q->mode), "%s", sub);
        }
        get(line, "RST_SENT", q->rst_s, sizeof(q->rst_s));
        get(line, "RST_RCVD", q->rst_r, sizeof(q->rst_r));
        get(line, "SOTA_REF", q->their_ref, sizeof(q->their_ref));
        if (!q->their_ref[0])
            get(line, "SIG_INFO", q->their_ref, sizeof(q->their_ref));
        n++;
    }
    fclose(f);
    if (n > max) {   /* the ring starts at n % max: turned so the oldest kept comes first */
        int start = n % max;
        reverse(out, 0, start - 1);
        reverse(out, start, max - 1);
        reverse(out, 0, max - 1);
    }
    return n;
}

/* n digits from p, and nothing after them */
static bool digits_end(const char *p, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (p[i] < '0' || p[i] > '9')
            return false;
    return p[n] == 0;
}

/* 3 to 4 digits, an optional letter, and nothing after: an ARLHS number's (079, 1286, 079H) */
static bool lh_number(const char *p) {
    size_t d = strspn(p, "0123456789");

    return d >= 3 && d <= 4 && (!p[d] || (p[d] >= 'A' && p[d] <= 'Z' && !p[d + 1]));
}

static bool three_letters(const char *p) {
    return p[0] >= 'A' && p[0] <= 'Z' && p[1] >= 'A' && p[1] <= 'Z' && p[2] >= 'A' && p[2] <= 'Z';
}

bool field_ref_lh(const char *in, char *out, size_t size) {
    char        s[24];
    size_t      i = 0;
    const char *p;

    for (; in && *in && i < sizeof(s) - 1; in++)
        s[i++] = (char)toupper((unsigned char)*in);
    s[i] = 0;
    p = s;
    if (!strncmp(p, "ARLHS", 5) && (p[5] == ' ' || p[5] == '-' || p[5] == '_'))
        p += 6;
    else if (!strncmp(p, "LH", 2) && (p[2] == ' ' || p[2] == '-'))
        p += 3;
    if (!three_letters(p))
        return false;
    const char *num = p + 3 + (p[3] == ' ' || p[3] == '-');
    size_t      d   = strspn(num, "0123456789");
    if (d < 1 || d > 4 || (num[d] && (num[d] < 'A' || num[d] > 'Z' || num[d + 1])))
        return false;
    snprintf(out, size, "%.3s %03d%s", p, atoi(num), num + d);
    return true;
}

/* A 6-character locator, in either case: JO20ab, JO20AB */
static bool tile(const char *p) {
    for (int i = 0; i < 6; i++) {
        char c = (char)toupper((unsigned char)p[i]);
        if (!(i >= 2 && i <= 3 ? c >= '0' && c <= '9' : c >= 'A' && c <= (i < 2 ? 'R' : 'X')))
            return false;
    }
    return p[6] == 0;
}

bool field_ref_tile(const char *in, char *out, size_t size) {
    if (!in || !tile(in))
        return false;
    snprintf(out, size, "%c%c%c%c%c%c", toupper((unsigned char)in[0]), toupper((unsigned char)in[1]), in[2], in[3],
             tolower((unsigned char)in[4]), tolower((unsigned char)in[5]));
    return true;
}

char field_ref_prog(const char *ref) {
    size_t      n = ref ? strlen(ref) : 0;
    const char *dash = n ? strchr(ref, '-') : NULL;

    if (n == 6 && ref[0] == 'X' && digits_end(ref + 1, 5))               /* X42018 */
        return 'M';
    if (n == 6 && tile(ref))                                             /* JO20ab: a Tiles tile */
        return 'G';
    if (n >= 7 && n <= 9 && three_letters(ref) && ref[3] == ' ' && lh_number(ref + 4))   /* BEL 002, USA 1286 */
        return 'L';
    if (n < 5 || n > 15 || !dash)
        return 0;
    for (const char *p = ref; *p; p++)
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '/' || *p == '-'))
            return 0;
    size_t pre = (size_t)(dash - ref);
    if (ref[0] == 'B' && ref[1] == '/')                                   /* B/G-0070 */
        return 'B';
    if (strchr(ref, '/'))                                                 /* I/LO-123, W7W/KG-001 */
        return 'S';
    if (dash[1] == 'H' && pre == 2)                                       /* RO-H0390 */
        return 'H';
    if (pre >= 3 && dash[-1] == 'F' && dash[-2] == 'F')                   /* DLFF-0123 */
        return 'F';
    /* LLPL-0001, LLIE-10001: LL and two letters, which no POTA prefix has; before the towers (LLAR-0001 ends in R) */
    if (pre == 4 && ref[0] == 'L' && ref[1] == 'L' && ref[2] >= 'A' && ref[2] <= 'Z' && ref[3] >= 'A' && ref[3] <= 'Z'
        && (digits_end(dash + 1, 4) || digits_end(dash + 1, 5)))
        return 'K';
    /* OKR-0150, 9MR-0101, K1R-0029: 2 to 4 before the R, which no POTA prefix has (KR-1308 is a park) */
    if (pre >= 3 && pre <= 5 && dash[-1] == 'R' && digits_end(dash + 1, 4))
        return 'T';
    /* DL-01156, I-14619, 3D2-00001: five digits after a prefix of 1 to 3. POTA's five-digit parks (FR-10234,
     * US-12345) never start with 0 and have no 1-letter prefix; the WCA's only castles past 09999 are Italy's */
    if (pre >= 1 && pre <= 3 && digits_end(dash + 1, 5) && (dash[1] == '0' || (pre == 1 && ref[0] == 'I')))
        return 'C';
    /* BEL-002, USA-1286: three letters, which no POTA prefix has; the towers ending in R come first */
    if (pre == 3 && three_letters(ref) && lh_number(dash + 1))
        return 'L';
    return 'P';                                                           /* US-1234, IT-0123, FR-10234 */
}

const char *field_prog_sig(char prog) {
    switch (prog) {
        case 'P': return "POTA";
        case 'B': return "WWBOTA";
        case 'H': return "HOTA";
        case 'F': return "WWFF";
        case 'T': return "TOTA";
        case 'C': return "WCA";
        case 'M': return "MOTA";
        case 'L': return "ARLHS";   /* the lighthouses' numbering; GMA takes the reference from its upload form */
        case 'G': return "TILES";   /* Tiles reads the tile from MY_GRIDSQUARE; MY_SIG names the activation here */
        case 'K': return "LLOTA";   /* llota.app finds the lake by MY_SIG LLOTA / MY_SIG_INFO in the upload */
        default:  return NULL;
    }
}

const char *field_qso_tag(const field_qso_t *q) {
    switch (field_ref_prog(q->their_ref)) {
        case 'S': return "S2S";
        case 'P': return "P2P";
        case 'F': return "F2F";
        case 'B': return "B2B";
        case 'H': return "H2H";
        case 'T': return "T2T";
        case 'C': return "C2C";
        case 'M': return "M2M";
        case 'L': return "L2L";
        case 'G': return "T3";      /* Tiles' own name for tile to tile */
        case 'K': return "LL2LL";   /* lake to lake: L2L is the lighthouses' */
        default:  return "";
    }
}
