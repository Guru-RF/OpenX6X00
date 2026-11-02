/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - FT8 CQ scheduler
 *
 *  Copyright (c) 2026
 */

/*
 *  Composes outgoing CQ TX messages. Currently exposes just the message
 *  builder; higher-level scheduling (when to actually queue a CQ for the
 *  next slot, pausing for an active QSO, etc.) lives in dialog_ft8.c for
 *  now. This header is a placeholder boundary for future moves.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The CQ as it goes on the air, into out (FTX_MAX_MESSAGE_LENGTH, caller's):
 *   "CQ <callsign> <grid>", "CQ <callsign>" with no QTH set (the grid the QTH's square, upper case: "JO21")
 *   "CQ_<mod> <callsign> <grid>"         (with a modifier: "CQ POTA ..." on the air)
 *   "CQ <callsign>"                      (a call that is not a base call, PA/ON6URE: a type 4 CQ, no room for
 *                                         the grid or the modifier)
 * Each is round-tripped through ftx_message_{encode,decode} and must come back the same. False, out empty: not even
 * "CQ <callsign>" goes (logged with LV_LOG_USER; the caller tells the user) */
bool cq_make_message(const char *callsign,
                     const char *qth,
                     const char *cq_mod,
                     char       *out);

#ifdef __cplusplus
}
#endif
