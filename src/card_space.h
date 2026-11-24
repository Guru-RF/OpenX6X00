/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the room left on the card (card_space.c, #341)
 *
 *  Recordings, screen recordings, pictures, logs and backups go to the card's DATA partition (BOARD_DATA) and
 *  nothing takes them away again. Settings > Info > STORAGE shows the room left there and on the system partition,
 *  and a start with less room on DATA than Low space warning says so once (card_space_notice.c).
 *
 *  The warning counts megabytes free, not a part of the card: a small card that is half full has room for hours
 *  of audio, a big one with 1 % left has too. On a DATA partition smaller than four times the choice a quarter of
 *  the partition takes its place, so a small partition does not warn from its first day. Here only the rule, the
 *  texts and the read; no LVGL, tools/card_space tests them.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Settings > Info > STORAGE > Low space warning (the setting card_space_warn), in the order of its choices */
typedef enum {
    CARD_WARN_OFF = 0,
    CARD_WARN_100MB,    /* the default: twice the room under which the screen recording stops (screen_rec.c) */
    CARD_WARN_250MB,
    CARD_WARN_500MB,
    CARD_WARN_1GB,
    CARD_WARN_LAST = CARD_WARN_1GB,
} card_warn_t;

#define CARD_WARN_DEFAULT CARD_WARN_100MB
#define CARD_WARN_CHOICES " Off \n Under 100 MB \n Under 250 MB \n Under 500 MB \n Under 1 GB "

/* The megabytes of a choice; 0 for Off and for a value that is none of them */
uint32_t card_warn_mb(int choice);

/* The bytes free under which the start warns, for a partition of total_bytes: the choice's megabytes, or a quarter
 * of the partition when that is less; 0: never (Off, or the size not known) */
uint64_t card_warn_under(uint64_t total_bytes, int choice);

/* True when a start warns: free_bytes under card_warn_under() */
bool card_space_low(uint64_t free_bytes, uint64_t total_bytes, int choice);

/* A size as shown: "0 MB", "85 MB", "999 MB", "1.0 GB", "9.9 GB", "29 GB", in 1024s, rounded down so that it never
 * shows more room than there is */
void card_space_text(uint64_t bytes, char *out, size_t size);

/* "1.2 GB free of 29 GB" */
void card_space_line(uint64_t free_bytes, uint64_t total_bytes, char *out, size_t size);

/* The room on the filesystem that holds path, as a user's files can take it; false when it cannot be read. On the
 * DATA partition (FAT) the first call after a start counts the whole table, which takes a moment on a big card:
 * card_space_notice_init() makes it from a thread of its own */
bool card_space_read(const char *path, uint64_t *free_bytes, uint64_t *total_bytes);

/* At the start (card_space_notice.c): the room on DATA read in a thread of its own, and once, 15 s after the start,
 * a message of 8 s when it is under the warning's choice. It never holds the start up and waits for no key */
void card_space_notice_init(void);

#ifdef __cplusplus
}
#endif
