/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the Internet radio's station lines (iradio_list.h)
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */
#include "iradio_list.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* src into dst (size bytes with the end), cut before a character that does not fit whole */
static void copy_utf8(char *dst, size_t size, const char *src) {
    size_t n = strlen(src);

    if (n >= size) {
        n = size - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xc0) == 0x80) n--;    /* src[n] starts the cut character */
    }
    memcpy(dst, src, n);
    dst[n] = 0;
}

/* The fields of a tab-separated line, the end of line taken off: how many (at most max) */
static int split(char *line, char **f, int max) {
    int n = 0;

    line[strcspn(line, "\r\n")] = 0;
    for (char *p = line; n < max;) {
        f[n++] = p;
        p      = strchr(p, '\t');
        if (!p) break;
        *p++ = 0;
    }
    return n;
}

static bool is_url(const char *s) {
    return !strncasecmp(s, "http://", 7) || !strncasecmp(s, "https://", 8);
}

bool iradio_parse_station(char *line, iradio_station_t *s) {
    char *f[6] = {0};
    int   n    = split(line, f, 6);

    if (n < 2 || !f[0][0] || !is_url(f[1]) || strlen(f[1]) >= sizeof(s->url) || strpbrk(f[1], " \"'")) return false;
    memset(s, 0, sizeof(*s));
    copy_utf8(s->name, sizeof(s->name), f[0]);
    strcpy(s->url, f[1]);
    if (n > 2) {
        for (size_t i = 0; f[2][i] && i < sizeof(s->codec) - 1; i++) s->codec[i] = toupper((unsigned char)f[2][i]);
    }
    if (n > 3 && strspn(f[3], "0123456789") == strlen(f[3]) && strlen(f[3]) < sizeof(s->kbps)) strcpy(s->kbps, f[3]);
    if (n > 4 && strlen(f[4]) == 2 && isalpha((unsigned char)f[4][0]) && isalpha((unsigned char)f[4][1])) {
        s->cc[0] = toupper((unsigned char)f[4][0]);
        s->cc[1] = toupper((unsigned char)f[4][1]);
    }
    if (n > 5 && strlen(f[5]) == 36 && strspn(f[5], "0123456789abcdef-") == 36) strcpy(s->uuid, f[5]);
    return true;
}

bool iradio_parse_country(char *line, iradio_country_t *c) {
    char *f[3] = {0};
    int   n    = split(line, f, 3);

    if (n < 2 || !f[0][0] || strlen(f[1]) != 2 || !isupper((unsigned char)f[1][0]) || !isupper((unsigned char)f[1][1]))
        return false;
    copy_utf8(c->name, sizeof(c->name), f[0]);
    for (char *p = c->name; *p; p++)
        if (*p == '#') *p = ' ';    /* the window's labels colour by #rrggbb */
    strcpy(c->cc, f[1]);
    c->stations = n > 2 ? (unsigned)strtoul(f[2], NULL, 10) : 0;
    return true;
}

bool iradio_playable(const iradio_station_t *s, bool every) {
    return every || !s->codec[0] || !strcmp(s->codec, "MP3");
}

void iradio_info(const iradio_station_t *s, bool every, char *buf, size_t size, const char *no) {
    char        kbps[12] = "";
    const char *part[3]  = {s->codec, kbps, iradio_playable(s, every) ? s->cc : no};
    size_t      used     = 0;

    if (s->kbps[0]) snprintf(kbps, sizeof(kbps), "%sk", s->kbps);
    buf[0] = 0;
    for (int i = 0; i < 3 && used < size; i++) {
        if (part[i][0]) used += snprintf(buf + used, size - used, "%s%s", used ? " " : "", part[i]);
    }
}

int iradio_load(const char *path, iradio_station_t *out, int max) {
    FILE *f = fopen(path, "r");
    char  line[512];
    int   n = 0;

    if (!f) return 0;
    while (n < max && fgets(line, sizeof(line), f)) {
        if (!strchr(line, '\n') && !feof(f)) {     /* a line too long for us: the rest of it skipped */
            int c;
            while ((c = fgetc(f)) != EOF && c != '\n') {}
            continue;
        }
        if (iradio_parse_station(line, &out[n])) n++;
    }
    fclose(f);
    return n;
}

/* path written whole by fill and put in place by a rename, on the card before: /mnt is FAT, written out later, and a
 * power cut in between left a short or empty list (channel_list.c). The folder is made when missing */
static bool write_whole(const char *path, bool (*fill)(FILE *f, const void *arg), const void *arg) {
    char tmp[300], buf[300], dir[300];

    snprintf(buf, sizeof(buf), "%s", path);
    snprintf(dir, sizeof(dir), "%s", dirname(buf));
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) return false;
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE *f = fopen(tmp, "w");
    if (!f) return false;

    bool ok = fill(f, arg);
    ok = fflush(f) == 0 && fsync(fileno(f)) == 0 && ok;
    ok = fclose(f) == 0 && ok;
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        return false;
    }
    int dfd = open(dir, O_RDONLY | O_DIRECTORY);
    if (dfd >= 0) {
        fsync(dfd);
        close(dfd);
    }
    return true;
}

typedef struct {
    const iradio_station_t *list;
    int                     n;
} stations_t;

static bool fill_stations(FILE *f, const void *arg) {
    const stations_t *st = arg;
    bool              ok = true;

    for (int i = 0; i < st->n && ok; i++) {
        const iradio_station_t *s = &st->list[i];
        ok = fprintf(f, "%s\t%s\t%s\t%s\t%s\t%s\n", s->name, s->url, s->codec, s->kbps, s->cc, s->uuid) > 0;
    }
    return ok;
}

bool iradio_save(const char *path, const iradio_station_t *list, int n) {
    stations_t st = {list, n};

    return write_whole(path, fill_stations, &st);
}

static bool fill_country(FILE *f, const void *arg) {
    const iradio_country_t *c = arg;

    return fprintf(f, "%s\t%s\n", c->name, c->cc) > 0;
}

bool iradio_save_country(const char *path, const iradio_country_t *c) {
    return write_whole(path, fill_country, c);
}

bool iradio_load_country(const char *path, iradio_country_t *c) {
    FILE *f = fopen(path, "r");
    char  line[160], old[160], *field[2] = {0};

    if (!f) return false;
    bool got = fgets(line, sizeof(line), f) != NULL;
    fclose(f);
    if (!got) return false;
    snprintf(old, sizeof(old), "%s", line);
    if (iradio_parse_country(line, c)) return true;
    /* The code first, the name after: as v0.3.13 wrote it, read as the countries' line it never was (#316) */
    if (split(old, field, 2) < 2) return false;
    snprintf(line, sizeof(line), "%s\t%s", field[1], field[0]);
    return iradio_parse_country(line, c);
}

int iradio_find(const iradio_station_t *list, int n, const char *url) {
    for (int i = 0; i < n; i++)
        if (!strcmp(list[i].url, url)) return i;
    return -1;
}
