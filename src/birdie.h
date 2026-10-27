/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - birdies: the steady carriers the radio makes inside itself (clock harmonics,
 *  the SDR's own spurs). Birdie Cal finds them with the antenna off and keeps their
 *  frequencies; the waterfall marks them, and the notch filter can be put on one that falls
 *  in the passband (Settings > Display > Birdies).
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#pragma once

#include "board.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BIRDIE_FILE BOARD_DATA "/birdies.txt"      /* one frequency in Hz a line, sorted */
#define BIRDIES_MAX 2048    /* 256 filled up in the first bands of a scan, the later ones got none (#50) */

/* Loads the file, takes the marks and the notch along with the frequency and the mode (LVGL
 * thread, once the settings are up) */
void birdie_init(void);

/* The known birdies, sorted; the ones between lo and hi */
int     birdie_count(void);
int32_t birdie_at(int i);
int     birdie_count_in(int32_t lo, int32_t hi);

/* Forgets them all (the file too) */
void birdie_clear(void);

/* The scan: every active band, with the antenna disconnected. progress() is called on the
 * LVGL thread with a line for the screen, done true at the end (also when stopped). One scan
 * at a time; false when one is running already. */
typedef void (*birdie_progress_fn)(const char *text, bool done, void *arg);
bool birdie_scan_start(birdie_progress_fn progress, void *arg);
void birdie_scan_stop(void);
bool birdie_scanning(void);

#ifdef __cplusplus
}
#endif
