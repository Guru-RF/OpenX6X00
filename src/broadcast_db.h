// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Xiegu X6100 LVGL GUI - broadcast stations from the EiBi CSV (broadcast_db.c)
//
// Copyright (c) 2026 Giovanni Ricciardi IU1WXY, own code of the IU1WXY firmware

#ifndef X6100_BROADCAST_DB_H
#define X6100_BROADCAST_DB_H

#include <stdbool.h>     /* broadcast_db_init() */
#include <stddef.h>      /* size_t */
#include <stdint.h>      /* int32_t */

#define BROADCAST_MAX_MATCHES   16      /* the most a lookup returns */

/* One schedule line; the text fields as the CSV has them, cut to these sizes */
struct broadcast_match {
    double  freq_khz;                   /* as the CSV has it, may have decimals */
    int     start_minute, stop_minute;  /* UTC, minutes of the day; 1440 is midnight at the end */
    char    station[202];               /* plain ASCII */
    char    language[50];               /* Lng column */
    char    target[63];                 /* Target column, the area served */
    char    days[60];                   /* EiBi's Days column, empty: every day */
    char    itu[50];                    /* ITU column, country of the transmitter */
    char    remarks[136];               /* Remarks column */
};
typedef struct broadcast_match broadcast_match_t;

/* Load /mnt/broadcast.csv, copied there from /usr/share/x6100 when missing. False without
 * stations */
bool broadcast_db_init(void);           /* once, from main_screen() */

/* The stations on air now (UTC day, time and season) within tolerance_hz of freq_hz, at most max */
size_t broadcast_db_find_matches(int32_t freq_hz, int tolerance_hz, broadcast_match_t *out, size_t max);

#endif /* X6100_BROADCAST_DB_H */
