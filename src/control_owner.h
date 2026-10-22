/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - Who controls the radio (control_owner.c)
 *
 *  One holder at a time for the actions that touch the RF side (frequency, mode, keys, PTT,
 *  the apps): the operator at the radio, a browser session or a TCI client. A browser takes
 *  control on purpose (R3); a TCI client takes it as it did before, unless someone is on the
 *  air; the radio's own keys and knobs always take it back (local priority). Nobody takes it
 *  while the radio transmits.
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

typedef enum { CTL_NONE = 0, CTL_LOCAL, CTL_BROWSER, CTL_TCI } ctl_kind_t;

typedef struct {
    ctl_kind_t kind;
    int        id;          /* the server's client index, or the session number */
    char       name[32];    /* the browser session's name, "TCI", "" */
    uint64_t   since_ms;
} ctl_info_t;

void ctl_init(void);

/* The holder asks to control: true if it controls now. take: a deliberate takeover, allowed
 * unless the radio is on the air; without it only a free lease, or one already held. */
bool ctl_claim(ctl_kind_t kind, int id, const char *name, bool take);
void ctl_release(ctl_kind_t kind, int id);      /* nothing unless this holder has it */
bool ctl_holds(ctl_kind_t kind, int id);
void ctl_get(ctl_info_t *out);
uint32_t ctl_version(void);                     /* one more at every change */

/* The radio's own keys and knobs: control comes back to the operator at the radio, unless the
 * event was one the remote server injected a moment ago (they come through the same devices) */
void ctl_local_input(void);
void ctl_injected(void);

/* On the air, by anyone (the radio's state) */
bool ctl_transmitting(void);

#ifdef __cplusplus
}
#endif
