/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  CW trainer: how it scores (dialog_cw_trainer.c). Copy (#290): the radio keys a group, the user types what was
 *  heard on a USB or Bluetooth keyboard (or picks it with the MFK); how a typed key becomes a place of the answer,
 *  and how the answer is checked against the group place by place. Learn and Copy: the last tries of every
 *  character and today's totals (Stats), and the last groups that take the level up. Shared with its host test
 *  (tools/cw_score).
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

#define CWT_CHARS  40   /* the characters of the Koch order (dialog_cw_trainer.c) */
#define CWT_HIST   20   /* tries kept per character */
#define CWT_WINDOW 10   /* the last groups the level's score is measured over */
#define CWT_PASS   90   /* per cent of them right for the next level */

/* Copy's answer */

/* The trainer's character for a key of the keyboard, as LVGL hands it over (the character typed, ASCII; a letter of
 * two bytes packed, Å): its capital when it is one of set (the trainer's 40 characters), else 0. A key that is no
 * character of the trainer types nothing: Space, Å, =, and LVGL's own keys (Enter, Backspace, the arrows) */
char cwt_copy_char(uint32_t key, const char *set);

/* c onto the answer, which holds at most max places (the group's length). False when it is full */
bool cwt_copy_add(char *answer, int max, char c);

/* The answer's last place rubbed out. False when it is empty */
bool cwt_copy_del(char *answer);

/* The answer typed against the group sent, lined up by the fewest changes that make one of the other (a place wrong,
 * missed, or typed one too many); place by place where that costs no more. Each place of the group: sent_ok[i] when
 * it was copied; each place of the answer: typed_ok[j] when it is right. A character missed or one too many then
 * costs that place, not the rest of the group: KMRSU typed KRSU missed M only, KMMRS had one M too many and U
 * missed. Typed KMRSA had U wrong, MKRSU K and M (swapped), IRK typed KSS all three. Returns the group's places
 * copied; the answer is right when that is all of them and the answer is as long as the group. Either array may be
 * NULL */
int cwt_copy_check(const char *sent, const char *typed, bool *sent_ok, bool *typed_ok);

/* The on-screen keys: n of them (the level's characters, then Del and OK), the one chosen moved by d, round from
 * the last to the first and back */
int cwt_copy_sel(int sel, int n, int d);

/* Stats, Learn's and Copy's each */

/* Every character's last CWT_HIST tries (bit 0 the newest, 1 = right), n of them; today's groups, right ones and
 * time */
typedef struct {
    uint32_t mask[CWT_CHARS];
    uint8_t  n[CWT_CHARS];
    uint32_t tries, right, ms;
} cwt_stats_t;

/* A try of character i */
void cwt_stats_add(cwt_stats_t *s, int i, bool ok);

/* How many of character i's last tries were right, in per cent; -1 when it was never tried */
int cwt_stats_pct(const cwt_stats_t *s, int i);

/* A group's result: a try today (right: the whole group was), and a try of each of its characters, right where
 * sent_ok says. set: the characters in the stats' order; one not in it is left out */
void cwt_stats_group(cwt_stats_t *s, const char *set, const char *sent, const bool *sent_ok, bool right);

/* This session's groups: all of them and the right ones, and the last CWT_WINDOW (1 = right, the newest over the
 * oldest) */
typedef struct {
    uint8_t hist[CWT_WINDOW];
    int     n, pos;
    int     tried, right;
} cwt_session_t;

void cwt_session_add(cwt_session_t *s, bool ok);

/* The right ones of the last CWT_WINDOW */
int cwt_session_good(const cwt_session_t *s);

/* CWT_WINDOW groups since the level was set, and CWT_PASS per cent of them right: the next level */
bool cwt_session_pass(const cwt_session_t *s);

/* The level changed: its window starts again, the session's totals stay */
void cwt_session_level(cwt_session_t *s);

#ifdef __cplusplus
}
#endif
