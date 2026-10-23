/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - Field Ops: the activation log
 */

#define _GNU_SOURCE

#include "flog.h"
#include "../board.h"

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

/* The ADIF SUBMODE of a log mode that is one, NULL when the mode stands on its own */
static const char *adif_submode(const char *mode) {
    static const char *const sub[] = {"FT4", "JS8", "FT2", NULL};

    for (int i = 0; sub[i]; i++)
        if (!strcasecmp(mode, sub[i]))
            return sub[i];
    return NULL;
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
    /* ADIF has FT4, JS8 and FT2 as submodes of MFSK (FT8 is a mode of its own) */
    const char *submode = adif_submode(q->mode);
    n = field(buf, size, n, "MODE", submode ? "MFSK" : q->mode);
    n = field(buf, size, n, "SUBMODE", submode ? submode : "");
    n = field(buf, size, n, "RST_SENT", q->rst_s);
    n = field(buf, size, n, "RST_RCVD", q->rst_r);
    n = field(buf, size, n, "NAME", q->name);
    n = field(buf, size, n, "QTH", q->qth);
    n = field(buf, size, n, "GRIDSQUARE", q->grid);
    n = field(buf, size, n, "COUNTRY", q->country);
    n = field(buf, size, n, "STATION_CALLSIGN", st->call);
    n = field(buf, size, n, "OPERATOR", st->call);
    n = field(buf, size, n, "MY_GRIDSQUARE", st->grid);
    if (st->prog == 'S') {
        n = field(buf, size, n, "MY_SOTA_REF", st->ref);
    } else if (st->prog == 'P') {
        n = field(buf, size, n, "MY_SIG", "POTA");
        n = field(buf, size, n, "MY_SIG_INFO", st->ref);
        n = field(buf, size, n, "MY_POTA_REF", st->ref);
    } else if (st->prog == 'B' || st->prog == 'H' || st->prog == 'F') {
        n = field(buf, size, n, "MY_SIG", st->prog == 'B' ? "WWBOTA" : st->prog == 'H' ? "HOTA" : "WWFF");
        n = field(buf, size, n, "MY_SIG_INFO", st->ref);
    }
    /* The other station's reference: park to park, summit to summit */
    char their = field_ref_prog(q->their_ref);
    if (their == 'S') {
        n = field(buf, size, n, "SOTA_REF", q->their_ref);
    } else if (their == 'P') {
        n = field(buf, size, n, "SIG", "POTA");
        n = field(buf, size, n, "SIG_INFO", q->their_ref);
        n = field(buf, size, n, "POTA_REF", q->their_ref);
    } else if (their) {
        n = field(buf, size, n, "SIG", their == 'B' ? "WWBOTA" : their == 'H' ? "HOTA" : "WWFF");
        n = field(buf, size, n, "SIG_INFO", q->their_ref);
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
        fprintf(f, BOARD_NAME " Field Ops log\n<ADIF_VER:5>3.1.4 <PROGRAMID:9>OpenX6X00 <eoh>\n");
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
        if (!strcasecmp(q->mode, "MFSK")) {       /* the submode names it: FT4, JS8, FT2 */
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

char field_ref_prog(const char *ref) {
    size_t      n = ref ? strlen(ref) : 0;
    const char *dash = n ? strchr(ref, '-') : NULL;

    if (n < 5 || n > 15 || !dash)
        return 0;
    for (const char *p = ref; *p; p++)
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '/' || *p == '-'))
            return 0;
    if (ref[0] == 'B' && ref[1] == '/')                                   /* B/G-0070 */
        return 'B';
    if (strchr(ref, '/'))                                                 /* I/LO-123, W7W/KG-001 */
        return 'S';
    if (dash[1] == 'H' && dash - ref == 2)                                /* RO-H0390 */
        return 'H';
    if (dash - ref >= 3 && dash[-1] == 'F' && dash[-2] == 'F')            /* DLFF-0123 */
        return 'F';
    return 'P';                                                           /* US-1234, IT-0123 */
}

const char *field_qso_tag(const field_qso_t *q) {
    switch (field_ref_prog(q->their_ref)) {
        case 'S': return "S2S";
        case 'P': return "P2P";
        case 'F': return "F2F";
        case 'B': return "B2B";
        case 'H': return "H2H";
        default:  return "";
    }
}
