/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - FT8/FT4 message text for any callsign
 *
 *  Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100), from its d10ae10
 */

#pragma once

#include <stdbool.h>

#include <ft8lib/message.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A call FT8/FT4 packs in 28 bits: a base call, with /P or /R at most. It goes with a grid or a report in one
 * message. Any other (KF0QGT/6Y, PA/ON6URE, ON6URE/MM, OT4ABCD) goes as plain text only in a "type 4" message, which
 * holds one more call (as a hash) and RRR, RR73 or 73, but no grid and no report; elsewhere it goes as a hash */
bool ftx_call_is_standard(const char *call);

/* ftx_message_encode(), and the type 4 form WSJT-X sends for a non-standard call of ours, "<W1ABC> KF0QGT/6Y" and
 * "<W1ABC> KF0QGT/6Y RR73": ft8_lib takes it only with the hashed call without its brackets. The message is zeroed
 * first */
ftx_message_rc_t ftx_encode_text(ftx_message_t *msg, ftx_callsign_hash_interface_t *hash_if, const char *text);

#ifdef __cplusplus
}
#endif
