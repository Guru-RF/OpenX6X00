/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - Zello: the radio on a channel of Zello, the push-to-talk voice service on the internet (dialog_zello.c)
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */
#pragma once

#include "dialog.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* While it is open the radio's PTT key talks on the channel and never keys the radio (vnet_ptt_key, voice_ptt.h) */
extern dialog_t *dialog_zello;

#ifdef __cplusplus
}
#endif
