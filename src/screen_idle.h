/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - when the screensaver comes and when the screen goes dark (screen_idle.c)
 *
 *  Three stages: the GUI lit, the screensaver over it, the panel dark. The minutes count from the last use: a key,
 *  a knob, the pointer, the hand mic, the browser panel, TCI or CAT tuning (whatever calls backlight_tick()). With a
 *  screensaver it comes after saver_min minutes, and the screen goes dark off_min minutes after the last use (0:
 *  never, the screensaver stays); a screen off time that is not later than the screensaver's comes first, with no
 *  screensaver. TX counts as use for the screensaver: it ends it at once, and while the radio is on the air the
 *  minutes do not run. The first use on the screensaver, or on a screen dark by the time, only wakes it. While
 *  someone watches the screen from a browser (the remote screen) the screensaver waits, they see the GUI; the dark
 *  comes at its time, as before.
 *
 *  Without a screensaver (SCREEN_SAVER_NONE) it is the screen off of before, exactly: dark after off_min minutes
 *  without a key or a knob, 0 never, TX changes nothing. always_off keeps the screen dark (a radio run from the knob
 *  or a browser): no screensaver, the keys work in the dark, POWER shows the screen for off_min minutes, one at
 *  least. The POWER key's tap turns a lit screen dark (and the screensaver with it) and a dark one lit.
 *
 *  Here only the decisions, from the settings and the times given; screen_light.c lights, darkens and shows the
 *  screensaver (screen_saver.c).
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The screensavers (params.screen_saver; Settings and the web Display page list them in this order) */
#define SCREEN_SAVER_NONE   0
#define SCREEN_SAVER_MATRIX 1       /* Matrix rain */
#define SCREEN_SAVER_LAST   SCREEN_SAVER_MATRIX

/* The defaults (params.c): the screensaver after 5 minutes, never dark. A radio whose screen off time was set
 * before the screensaver came keeps it (migrations.c, _16) */
#define SCREEN_SAVER_DEFAULT     SCREEN_SAVER_MATRIX
#define SCREEN_SAVER_MIN_DEFAULT 5
#define SCREEN_OFF_MIN_DEFAULT   0
#define SCREEN_MIN_MAX           120     /* the settings' top, minutes */

typedef enum {
    SCREEN_LIT,         /* the GUI */
    SCREEN_SAVER,       /* the screensaver over it, the panel lit */
    SCREEN_DARK,        /* the panel dark: the time, POWER or always off */
} screen_stage_t;

typedef struct {
    /* The settings: screen_idle_settings() after a change */
    uint8_t  saver;         /* SCREEN_SAVER_* */
    uint16_t saver_min;     /* the screensaver after this many minutes without use, 1 at least */
    uint16_t off_min;       /* the screen dark after this many minutes without use, 0 never */
    bool     always_off;
    /* The state */
    screen_stage_t stage;
    bool     power_off;     /* dark by the POWER key: until it again */
    bool     tx;            /* the radio on the air */
    bool     viewers;       /* the screen watched from a browser */
    uint64_t use_ms;        /* the last use: the minutes count from it */
    uint64_t shown_ms;      /* always off: when POWER showed the screen */
} screen_idle_t;

/* The settings in, at the start: lit, or dark when always off */
void screen_idle_start(screen_idle_t *s, uint64_t now_ms);

/* A use: the minutes again. True when it woke the screen, from the screensaver or from a screen dark by the time:
 * the key was for waking, not for the radio. A screen dark on purpose (POWER, always off) stays dark: false */
bool screen_idle_use(screen_idle_t *s, uint64_t now_ms);

/* The radio on the air or back on receive: with a screensaver it ends it, and the minutes run from the end of TX */
void screen_idle_tx(screen_idle_t *s, bool on, uint64_t now_ms);

/* Someone watches the screen from a browser, or nobody any more */
void screen_idle_viewers(screen_idle_t *s, bool any);

/* The POWER key's tap: dark when lit or under the screensaver, lit when dark */
void screen_idle_power(screen_idle_t *s, uint64_t now_ms);

/* The settings changed (Settings, the web configurator): applied now, the minutes again */
void screen_idle_settings(screen_idle_t *s, uint64_t now_ms);

/* The minutes: every so often, the stage they lead to */
void screen_idle_tick(screen_idle_t *s, uint64_t now_ms);

#ifdef __cplusplus
}
#endif
