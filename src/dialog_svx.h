/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - SVXConnect: the radio on an SvxLink reflector's talkgroups over the internet (dialog_svx.c)
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */
#pragma once

#include "dialog.h"

#ifdef __cplusplus
extern "C" {
#endif

/* While it is open the radio's PTT key talks on the selected talkgroup and never keys the radio (vnet_ptt_key) */
extern dialog_t *dialog_svx;

#ifdef __cplusplus
}
#endif
