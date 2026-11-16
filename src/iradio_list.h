/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the Internet radio's station lines (dialog_iradio.c): the lists rootfs/usr/sbin/x6100-iradio prints
 *  and the favourites file (/mnt/iradio/favorites.tsv, also written by the web configurator's Internet radio page),
 *  one station a line: name<TAB>address<TAB>codec<TAB>kbps<TAB>country code<TAB>uuid, the last four may be empty.
 *  Countries: name<TAB>code<TAB>stations. Nothing here touches LVGL, for tools/iradio's test.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char name[64];      /* UTF-8, cut on a whole character */
    char url[256];      /* http:// or https://; a longer one is no station (a cut address does not play) */
    char codec[9];      /* in capitals: MP3, AAC, AAC+, OGG, FLAC, HLS; "" not known (an address typed in) */
    char kbps[8];
    char cc[3];         /* ISO 3166-1, "" none */
    char uuid[40];      /* radio-browser.info's, for its count of what is played; "" none */
} iradio_station_t;

typedef struct {
    char     name[64];
    char     cc[3];
    unsigned stations;      /* MP3 or not: the directory counts them all */
} iradio_country_t;

/* One line (changed in place) into s: false for no name or no http(s) address */
bool iradio_parse_station(char *line, iradio_station_t *s);
bool iradio_parse_country(char *line, iradio_country_t *c);

/* MP3 plays everywhere (mpg123); every other codec only with every (ffmpeg on the card image): without it a station of
 * another codec is shown, marked, and not played. A codec not known (an address typed in) is tried */
bool iradio_playable(const iradio_station_t *s, bool every);

/* The list's second column: "MP3 128k BE"; no in the place of the country for one not played ("AAC 64k not playable") */
void iradio_info(const iradio_station_t *s, bool every, char *buf, size_t size, const char *no);

/* The favourites: at most max read; written whole and put in place by a rename, on the card before (FAT).
 * The folder is made when missing */
int  iradio_load(const char *path, iradio_station_t *out, int max);
bool iradio_save(const char *path, const iradio_station_t *list, int n);
int  iradio_find(const iradio_station_t *list, int n, const char *url);

#ifdef __cplusplus
}
#endif
