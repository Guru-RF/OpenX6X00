/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2024 Georgy Dyuldin aka R2RFE
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "adif.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <ctype.h>
#include <strings.h>

#define MHZ 1000000
#define KHZ 1000
#define ADIF_MAX_BYTES (64u << 20)   /* an import is read whole: 64 MB is more log than the radio holds */

#define COPY_STR(dst, src, len) (copy_str(dst, src, len, sizeof(dst)))

struct adif_log_s {
    FILE *fd;
};

static void write_header(FILE *fd);

static void copy_str(char * dst, char * src, size_t val_len, size_t dst_len);
static char * extract_str(const char * src, size_t src_len);

static qso_log_band_t str_to_band(const char * s);
static qso_log_mode_t create_mode(const char * mode, const char * submode);


adif_log adif_log_init(const char * path) {
    adif_log log = (adif_log) malloc(sizeof(struct adif_log_s));
    bool new_file = false;
    if (access(path, F_OK) != 0) {
        new_file = true;
    }
    FILE *log_fd = fopen(path, "a");
    if (log_fd == NULL) {
        perror("Unable to open log file:");
        free(log);
        return NULL;
    } else {
        log->fd = log_fd;
        if (new_file) {
            write_header(log->fd);
        }
    }
    return log;
}

void adif_log_close(adif_log l) {
    if (!l) return;     /* /mnt/ft_log.adi could not be opened (read-only or full microSD) */
    fclose(l->fd);
}

field_qso_t adif_field_qso(const qso_log_record_t *r) {
    /* The log's modes by their names in field_qso_t: field_adif_record() writes FT4, FT2 and JS8 as MFSK with
     * that SUBMODE. MODE_OTHER has none, and goes without a MODE */
    static const char *const names[] = {
        [MODE_SSB] = "SSB", [MODE_AM] = "AM",   [MODE_FM] = "FM",     [MODE_CW] = "CW",   [MODE_FT8] = "FT8",
        [MODE_FT4] = "FT4", [MODE_RTTY] = "RTTY", [MODE_JS8] = "JS8", [MODE_FT2] = "FT2",
    };
    /* The dial in Hz as the day's log always took it: float arithmetic, which gives the dial frequencies back
     * whole (7074000, 28074000) */
    field_qso_t q = {.t = r->time, .freq_hz = (int32_t)(r->freq_mhz * 1e6f + 0.5f)};
    /* The digital reports signed in two digits ("+01", "-05"), as in every logbook; the others as they are (599) */
    bool digi = r->mode == MODE_FT8 || r->mode == MODE_FT4 || r->mode == MODE_FT2 || r->mode == MODE_JS8;

    if ((unsigned)r->mode < sizeof(names) / sizeof(names[0]) && names[r->mode])
        snprintf(q.mode, sizeof(q.mode), "%s", names[r->mode]);
    snprintf(q.call, sizeof(q.call), "%s", r->remote_call);
    snprintf(q.rst_s, sizeof(q.rst_s), digi ? "%+03d" : "%d", r->rsts);
    snprintf(q.rst_r, sizeof(q.rst_r), digi ? "%+03d" : "%d", r->rstr);
    snprintf(q.name, sizeof(q.name), "%s", r->name);
    snprintf(q.qth, sizeof(q.qth), "%s", r->qth);
    snprintf(q.grid, sizeof(q.grid), "%s", r->remote_grid);
    return q;
}

/* The record the day's log has for the same QSO (dialog_field_day_log): the time to the second, the band as
 * ADIF writes it ("40m"), the frequency in MHz to the Hz, no empty fields. A logbook or LoTW given both files
 * finds the same QSO twice, not two QSOs a minute apart (#174). The other station's distance is left to the
 * logbook, from GRIDSQUARE and MY_GRIDSQUARE */
void adif_add_qso(adif_log l, qso_log_record_t qso)
{
    field_station_t sta = {0};
    field_qso_t     q;
    char            rec[768];

    if (!l) return;     /* the day's log says why (dialog_field_day_log) */
    q = adif_field_qso(&qso);
    snprintf(sta.call, sizeof(sta.call), "%s", qso.local_call);
    snprintf(sta.grid, sizeof(sta.grid), "%s", qso.local_grid);
    field_adif_record(rec, sizeof(rec), &q, &sta);
    fputs(rec, l->fd);
    fflush(l->fd);
}

/* One ADIF tag: <NAME:LEN[:TYPE]> or <EOR>/<EOH>. p is at '<'. TAG_OK with *next after the
 * value (or the tag); TAG_NONE for a '<' that starts no tag; TAG_BROKEN for a length that
 * runs past the end of the file: the record it is in cannot be trusted */
typedef struct {
    const char *name;
    size_t      name_len;
    const char *val;        /* NULL for a tag without a length (EOR, EOH) */
    size_t      val_len;
} adif_tag_t;

enum { TAG_NONE, TAG_OK, TAG_BROKEN };

static int tag_parse(const char *p, const char *end, adif_tag_t *t, const char **next) {
    const char *q = p + 1;

    t->name = q;
    while (q < end && *q != ':' && *q != '>' && !isspace((unsigned char)*q))
        q++;
    if (q >= end || q == t->name)
        return TAG_NONE;
    t->name_len = q - t->name;
    if (*q == '>') {
        t->val = NULL;
        t->val_len = 0;
        *next = q + 1;
        return TAG_OK;
    }
    if (*q != ':')
        return TAG_NONE;
    /* The length's digits, within the file: strtoul() on the buffer, which has no terminating
     * NUL, read past a file that ends in them (audit B04) */
    q++;
    if (q >= end || !isdigit((unsigned char)*q))
        return TAG_NONE;
    t->val_len = 0;
    while (q < end && isdigit((unsigned char)*q)) {
        t->val_len = t->val_len * 10 + (size_t)(*q - '0');
        if (t->val_len > ADIF_MAX_BYTES)
            return TAG_BROKEN;
        q++;
    }
    while (q < end && *q != '>')    /* the type, if any: not needed */
        q++;
    if (q >= end)
        return TAG_NONE;
    q++;
    /* The value must lie within the file: a length past its end is a broken file, and
     * copying it read past the buffer (audit A09) */
    if (t->val_len > (size_t)(end - q))
        return TAG_BROKEN;
    t->val = q;
    *next = q + t->val_len;
    return TAG_OK;
}

static bool tag_is(const adif_tag_t *t, const char *name) {
    return strlen(name) == t->name_len && strncasecmp(t->name, name, t->name_len) == 0;
}

/* The value as a C string, for the number and date parsers */
static void tag_copy(const adif_tag_t *t, char *buf, size_t size) {
    size_t n = t->val_len < size - 1 ? t->val_len : size - 1;

    memcpy(buf, t->val, n);
    buf[n] = 0;
}

/* Reads the records of an ADIF file into *records (malloc'd), whatever the line endings and
 * however the tags are laid out: the fields are found by their declared lengths, the record
 * ends at <EOR>, the header (if any) at <EOH>; tag names in either case, typed tags
 * (<CALL:5:S>) too. The local call is OPERATOR, else STATION_CALLSIGN (audit A15).
 * A record with a field length past the end of the file is left out and counted in
 * *skipped (when given). Returns the number of records, -1 when the file could not be read */
int adif_read(const char * path, qso_log_record_t ** records, int *skipped) {
    FILE    *fp = fopen(path, "r");
    char    *data = NULL;
    size_t  size = 0, got = 0;

    *records = NULL;
    if (skipped) *skipped = 0;
    if (fp == NULL) {
        perror("Unable to open log file:");
        return -1;
    }
    if (fseek(fp, 0, SEEK_END) == 0) {
        long l = ftell(fp);
        size = l > 0 ? (size_t)l : 0;
        rewind(fp);
    }
    if (size == 0 || size > ADIF_MAX_BYTES || !(data = malloc(size))) {
        fclose(fp);
        return 0;
    }
    got = fread(data, 1, size, fp);
    fclose(fp);

    const char *p = data, *end = data + got;
    size_t      arr_size = 128, count = 0;
    bool        in_header = true;    /* until <EOH>; a file without one starts with a record */
    bool        broken = false;      /* this record had a field that ran past the file */
    char        local[32], station[32], mode[32], submode[32], num[32];
    struct tm   qso_ts;
    qso_log_record_t *cur;

    *records = malloc(arr_size * sizeof(qso_log_record_t));
    cur = &(*records)[0];
    memset(cur, 0, sizeof(*cur));
    memset(&qso_ts, 0, sizeof(qso_ts));
    local[0] = station[0] = mode[0] = submode[0] = 0;

    /* A file that opens with a tag has no header */
    while (p < end && isspace((unsigned char)*p)) p++;
    if (p < end && *p == '<') {
        adif_tag_t  t;
        const char *next;
        if (tag_parse(p, end, &t, &next) == TAG_OK && !tag_is(&t, "EOH") && t.val)
            in_header = false;
    }

    while (p < end) {
        adif_tag_t t;
        const char *next;
        int         r;

        if (*p != '<') {
            p++;
            continue;
        }
        r = tag_parse(p, end, &t, &next);
        if (r != TAG_OK) {
            if (r == TAG_BROKEN) broken = true;
            p++;
            continue;
        }
        p = next;
        if (in_header) {
            if (tag_is(&t, "EOH")) in_header = false;
            continue;
        }
        if (tag_is(&t, "EOR")) {
            if (broken) {
                /* not a record to trust: left out, the next one starts clean */
                if (skipped) (*skipped)++;
                broken = false;
                memset(cur, 0, sizeof(*cur));
                memset(&qso_ts, 0, sizeof(qso_ts));
                local[0] = station[0] = mode[0] = submode[0] = 0;
                continue;
            }
            /* the record is complete */
            if (local[0]) COPY_STR(cur->local_call, local, strlen(local));
            else if (station[0]) COPY_STR(cur->local_call, station, strlen(station));
            cur->time = timegm(&qso_ts);
            cur->mode = create_mode(mode[0] ? mode : NULL, submode[0] ? submode : NULL);
            if ((qso_log_freq_to_band(cur->freq_mhz * MHZ) != cur->band) &&
                (qso_log_freq_to_band(cur->freq_mhz * KHZ) == cur->band)) {
                cur->freq_mhz /= 1000;      /* a log that wrote kHz */
            }
            count++;
            if (count >= arr_size) {
                arr_size *= 2;
                *records = realloc(*records, arr_size * sizeof(qso_log_record_t));
            }
            cur = &(*records)[count];
            memset(cur, 0, sizeof(*cur));
            memset(&qso_ts, 0, sizeof(qso_ts));
            local[0] = station[0] = mode[0] = submode[0] = 0;
            continue;
        }
        if (!t.val || t.val_len == 0)
            continue;
        if (tag_is(&t, "OPERATOR")) {
            tag_copy(&t, local, sizeof(local));
        } else if (tag_is(&t, "STATION_CALLSIGN")) {
            tag_copy(&t, station, sizeof(station));
        } else if (tag_is(&t, "CALL")) {
            COPY_STR(cur->remote_call, (char *)t.val, t.val_len);
        } else if (tag_is(&t, "QSO_DATE")) {
            tag_copy(&t, num, sizeof(num));
            strptime(num, "%Y%m%d", &qso_ts);
        } else if (tag_is(&t, "TIME_ON")) {
            tag_copy(&t, num, sizeof(num));
            strptime(num, strlen(num) >= 6 ? "%H%M%S" : "%H%M", &qso_ts);
        } else if (tag_is(&t, "MODE")) {
            tag_copy(&t, mode, sizeof(mode));
        } else if (tag_is(&t, "SUBMODE")) {
            tag_copy(&t, submode, sizeof(submode));
        } else if (tag_is(&t, "NAME")) {
            COPY_STR(cur->name, (char *)t.val, t.val_len);
        } else if (tag_is(&t, "QTH")) {
            COPY_STR(cur->qth, (char *)t.val, t.val_len);
        } else if (tag_is(&t, "RST_SENT")) {
            tag_copy(&t, num, sizeof(num));
            cur->rsts = atoi(num);
        } else if (tag_is(&t, "RST_RCVD")) {
            tag_copy(&t, num, sizeof(num));
            cur->rstr = atoi(num);
        } else if (tag_is(&t, "BAND")) {
            tag_copy(&t, num, sizeof(num));
            cur->band = str_to_band(num);
        } else if (tag_is(&t, "FREQ")) {
            tag_copy(&t, num, sizeof(num));
            cur->freq_mhz = strtof(num, NULL);
        } else if (tag_is(&t, "MY_GRIDSQUARE")) {
            COPY_STR(cur->local_grid, (char *)t.val, t.val_len);
        } else if (tag_is(&t, "GRIDSQUARE")) {
            COPY_STR(cur->remote_grid, (char *)t.val, t.val_len);
        }
    }
    if (broken && skipped) (*skipped)++;    /* a last record without its <EOR> */
    free(data);
    return (int)count;
}

static void write_header(FILE *fd) {
    fprintf(fd, "<PROGRAMID:9>OpenX6X00\r\n");
    fprintf(fd, "<PROGRAMVERSION:5>1.0.0\r\n");
    fprintf(fd, "<ADIF_VER:4>3.14\r\n");
    fprintf(fd, "<EOH>\r\n");
}

static void copy_str(char * dst, char * src, size_t val_len, size_t dst_len) {
    if (val_len > (dst_len - 1)) {
        val_len = dst_len - 1;
    }
    strncpy(dst, src, val_len);
    dst[val_len] = 0;
}

static char * extract_str(const char * src, size_t src_len) {
    char * dst;
    dst = malloc(src_len + 1);
    memcpy(dst, src, src_len);
    dst[src_len] = 0;
    return dst;
}

static qso_log_band_t str_to_band(const char * s) {
    return atoi(s);
}


static qso_log_mode_t create_mode(const char * mode, const char * submode) {
    if (!mode) return MODE_OTHER;
    if (strcmp(mode, "SSB") == 0) return MODE_SSB;
    if (strcmp(mode, "AM") == 0) return MODE_AM;
    if (strcmp(mode, "FM") == 0) return MODE_FM;
    if (strcmp(mode, "CW") == 0) return MODE_CW;
    if (strcmp(mode, "FT8") == 0) return MODE_FT8;
    if (strcmp(mode, "RTTY") == 0) return MODE_RTTY;
    if (!submode) return MODE_OTHER;
    if ((strcmp(mode, "MFSK") == 0) && (strcmp(submode, "FT4") == 0)) return MODE_FT4;
    if (!strcmp(mode, "MFSK") && !strcmp(submode, "JS8")) return MODE_JS8;
    if ((strcmp(mode, "MFSK") == 0) && (strcmp(submode, "FT2") == 0)) return MODE_FT2;
    return MODE_OTHER;
}
