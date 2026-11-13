/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  The dial frequencies the keyboard modes go to and step through with the band keys (PSK31 and PSK63, Olivia and
 *  Contestia, JTTY; #205): a built-in list for each, with the frequencies of the IARU region of the band plan in use
 *  where the activity differs (PSK on 40 m: 7.040 MHz in Region 1, 7.070 in Region 2 and the US, 7.035 in Region 3),
 *  and the user's own frequencies from the web configurator's Digital modes page (rows of the digital_modes table of
 *  the app's type), each in the place of the built-in ones on its band. tools/digi_freqs checks the lists.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#pragma once

#include "cfg/digital_modes.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DIGI_FREQS_MAX 24   /* entries of one app's list, the user's included */

typedef struct {
    int32_t freq;       /* dial, Hz, USB */
    char    label[32];  /* "40m", or the label of the user's own entry */
    bool    own;        /* the user's, from the Digital modes page */
} digi_freq_t;

/* The built-in list of type (CFG_DIG_TYPE_PSK, _OLIVIA or _JTTY; another type has none) for the IARU region (1, 2 or 3;
 * anything else is taken as 1), in frequency order: how many went into out, at most max */
int digi_freqs_builtin(cfg_digital_type_t type, int region, digi_freq_t *out, int max);

/* The built-in list with the user's n entries (own) in it: an entry on an amateur band takes the place of every
 * built-in one on that band, one on no band is added; an entry of 0 Hz or less, or on a frequency already in the list,
 * is left out. An entry without a label gets the label of the built-in one it replaced. In frequency order, at most
 * max (the highest go first when there are more) */
int digi_freqs_merge(cfg_digital_type_t type, int region, const digi_freq_t *own, int n, digi_freq_t *out, int max);

/* What the app steps through now: the merge for the band plan's region with the user's entries in params.db. Read
 * each time, so a change on the web page counts at the next band key or the next opening. Any thread */
int digi_freqs_get(cfg_digital_type_t type, digi_freq_t *out, int max);

/* The entry to go to from freq in list (n entries, in frequency order): dir 0 the closest (the lower of two as
 * close), above 0 the first above freq (past the last: the first), below 0 the last below freq (before the first:
 * the last). -1 for an empty list */
int digi_freqs_step(const digi_freq_t *list, int n, int32_t freq, int dir);

#ifdef __cplusplus
}
#endif
