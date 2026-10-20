/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  DX cluster spots: line parser, spot store, band and mode helpers.
 *  No LVGL and no network here, so it can be unit tested on a PC.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CLUSTER_CALL_LEN    16
#define CLUSTER_COMMENT_LEN 48
#define CLUSTER_MAX_SPOTS   300

typedef struct {
    int32_t  freq_hz;
    char     dx[CLUSTER_CALL_LEN];
    char     spotter[CLUSTER_CALL_LEN];
    char     comment[CLUSTER_COMMENT_LEN];
    uint16_t utc_min;   /* minutes after 00:00Z, from the spot's HHMMZ */
    int64_t  when;      /* UTC seconds when the line has a date (sh/dx rows), else 0 */
} cluster_spot_t;

typedef struct {
    cluster_spot_t spots[CLUSTER_MAX_SPOTS];
    int            count;
} cluster_store_t;

typedef enum {
    CLUSTER_MODE_CW,
    CLUSTER_MODE_LSB,
    CLUSTER_MODE_USB,
    CLUSTER_MODE_DIGI,
    CLUSTER_MODE_AM,
    CLUSTER_MODE_FM,
} cluster_mode_t;

/* One line from the cluster: a live "DX de ..." spot or a "sh/dx" row.
 * Returns false for anything else (prompts, announcements, WWV...). */
bool cluster_parse_line(const char *line, cluster_spot_t *out);

/* Adds a spot, keeping the store sorted by time (oldest first). The same DX
 * within 1 kHz keeps only the newest spot; when full the oldest is dropped. */
void cluster_store_add(cluster_store_t *s, const cluster_spot_t *spot);

/* Drops spots older than max_age_min, by their UTC time. */
void cluster_store_expire(cluster_store_t *s, uint16_t now_utc_min, uint16_t max_age_min);


/* Age of a spot in minutes (0..1439), across midnight. */
uint16_t cluster_age_min(uint16_t spot_utc_min, uint16_t now_utc_min);

/* Amateur band index of a frequency, or -1 outside the bands. */
int         cluster_band_of(int32_t hz);
const char *cluster_band_name(int band);

/* Mode to tune a spot with: from its comment (CW, FT8, SSB, ...), else
 * from the band plan (IARU Region 1). */
cluster_mode_t cluster_guess_mode(const cluster_spot_t *spot);

#ifdef __cplusplus
}
#endif
