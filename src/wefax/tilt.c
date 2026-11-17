/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - WeFax tilt, found on a chart and kept per station (tilt.h).
 *
 *  For each tilt tried, the rows are shifted as Fix would and summed column by column: lined up, the vertical
 *  lines make the sums jump from one column to the next, so the sum of the squared jumps is largest. The rows'
 *  own darkness is taken off first, a row of noise weighs no more than a row of chart. Every 8 over every third
 *  row, then each one around the best over all of them: tried on the charts of F4OLI's radio, DWD's came out
 *  -184 to -187 and straight, Northwood's -9 to -12.
 */

#include "tilt.h"

#include "../board.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TILT_FILE       BOARD_DATA "/wefax/tilt.conf"
#define STATIONS_MAX    16
#define KEEP            5       /* a station's last charts, its tilt their median */

/* The shift of row y (as wefax_png_write_fixed and the live view's tilt_px): the newest row stays */
static int shift_of(int y, unsigned h, unsigned w, int tilt)
{
    return (int)(((int64_t)y - (int64_t)(h - 1)) * tilt * WEFAX_TILT_UNIT_W / (1000LL * w));
}

static int64_t sharpness(const int16_t *dark, unsigned w, unsigned h, int tilt, unsigned step, int32_t *prof)
{
    memset(prof, 0, sizeof(*prof) * w);
    for (unsigned y = 0; y < h; y += step) {
        const int16_t *r  = dark + (size_t)y * w;
        int            sh = shift_of((int)y, h, w, tilt) % (int)w;

        if (sh < 0)
            sh += (int)w;
        for (unsigned x = 0; x < w; x++) {
            unsigned to = x + (unsigned)sh;

            prof[to >= w ? to - w : to] += r[x];
        }
    }

    int64_t s = 0;

    for (unsigned x = 0; x < w; x++) {
        int64_t d = (int64_t)prof[x + 1 < w ? x + 1 : 0] - prof[x];

        s += d * d;
    }
    return s;
}

bool wefax_auto_tilt(const uint8_t *img, unsigned w, unsigned h, int *tilt)
{
    if (!img || w < 16 || h < 16 || h > 4000)
        return false;

    int16_t *dark = malloc(sizeof(*dark) * w * h);
    int32_t *prof = malloc(sizeof(*prof) * w);

    if (!dark || !prof) {
        free(dark);
        free(prof);
        return false;
    }
    for (unsigned y = 0; y < h; y++) {
        const uint8_t *r   = img + (size_t)y * w;
        int16_t       *d   = dark + (size_t)y * w;
        uint32_t       sum = 0;

        for (unsigned x = 0; x < w; x++)
            sum += 255 - r[x];

        int mean = (int)(sum / w);

        for (unsigned x = 0; x < w; x++)
            d[x] = (int16_t)(255 - r[x] - mean);
    }

    int     best = 0;
    int64_t top  = -1;

    for (int t = -WEFAX_TILT_MAX; t <= WEFAX_TILT_MAX; t += 8) {
        int64_t s = sharpness(dark, w, h, t, 3, prof);

        if (s > top)
            top = s, best = t;
    }

    int coarse = best;

    top = -1;
    for (int t = coarse - 8; t <= coarse + 8; t++) {
        if (t < -WEFAX_TILT_MAX || t > WEFAX_TILT_MAX)
            continue;

        int64_t s = sharpness(dark, w, h, t, 1, prof);

        if (s > top)
            top = s, best = t;
    }
    free(dark);
    free(prof);
    if (best <= -WEFAX_TILT_MAX + 8 || best >= WEFAX_TILT_MAX - 8)
        return false;   /* pinned at the end: no lines to go by */
    *tilt = best;
    return true;
}

/* ---- per station: BOARD_DATA/wefax/tilt.conf, a line each, "<station> <tilt> <tilt>...", the newest last ---- */

typedef struct {
    char name[16];
    int  v[KEEP];
    int  n;
} station_t;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static int load(station_t *st)
{
    FILE *f = fopen(TILT_FILE, "r");
    char  line[160];
    int   n = 0;

    if (!f)
        return 0;
    while (n < STATIONS_MAX && fgets(line, sizeof(line), f)) {
        station_t *s = &st[n];
        char      *p = line, *end;

        memset(s, 0, sizeof(*s));
        if (sscanf(p, "%15s", s->name) != 1)
            continue;
        p += strcspn(p, " \t");
        while (s->n < KEEP) {
            long v = strtol(p, &end, 10);

            if (end == p)
                break;
            if (v >= -WEFAX_TILT_MAX && v <= WEFAX_TILT_MAX)
                s->v[s->n++] = (int)v;
            p = end;
        }
        if (s->n)
            n++;
    }
    fclose(f);
    return n;
}

static int cmp_int(const void *a, const void *b)
{
    return *(const int *)a - *(const int *)b;
}

static int median(const station_t *s)
{
    int v[KEEP];

    memcpy(v, s->v, sizeof(int) * s->n);
    qsort(v, s->n, sizeof(int), cmp_int);
    return v[s->n / 2];
}

bool wefax_station_tilt(const char *station, int *tilt)
{
    station_t st[STATIONS_MAX];
    bool      found = false;

    if (!station || !station[0])
        return false;
    pthread_mutex_lock(&lock);

    int n = load(st);

    for (int i = 0; i < n && !found; i++)
        if (!strcmp(st[i].name, station)) {
            *tilt = median(&st[i]);
            found = true;
        }
    pthread_mutex_unlock(&lock);
    return found;
}

void wefax_station_tilt_learn(const char *station, int tilt)
{
    station_t st[STATIONS_MAX];
    int       i;

    if (!station || !station[0] || strchr(station, ' '))
        return;
    pthread_mutex_lock(&lock);

    int n = load(st);

    for (i = 0; i < n && strcmp(st[i].name, station); i++)
        ;
    if (i == n) {
        if (n == STATIONS_MAX) {
            pthread_mutex_unlock(&lock);
            return;
        }
        memset(&st[n], 0, sizeof(st[n]));
        snprintf(st[n].name, sizeof(st[n].name), "%s", station);
        n++;
    }
    if (st[i].n == KEEP) {
        memmove(st[i].v, st[i].v + 1, sizeof(int) * (KEEP - 1));
        st[i].n--;
    }
    st[i].v[st[i].n++] = tilt;

    FILE *f = fopen(TILT_FILE ".tmp", "w");

    if (f) {
        for (int k = 0; k < n; k++) {
            fprintf(f, "%s", st[k].name);
            for (int j = 0; j < st[k].n; j++)
                fprintf(f, " %d", st[k].v[j]);
            fputc('\n', f);
        }
        if (fclose(f) == 0)
            rename(TILT_FILE ".tmp", TILT_FILE);
        else
            remove(TILT_FILE ".tmp");
    }
    printf("WEFAX TILT: %s %+d, its tilt now %+d\n", station, tilt, median(&st[i]));
    pthread_mutex_unlock(&lock);
}
