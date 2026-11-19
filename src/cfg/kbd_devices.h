/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - What each keyboard's [ ] keys are, device by device (kbd_devices.c, #306)
 *
 *  On a keyboard [ and ] are the VOL knob's keys, and in CW the paddle (paddle_keyer.c). A Bluetooth volume
 *  knob or a macro pad set up to turn the volume with them keys the transmitter in CW. Each USB or Bluetooth
 *  device can be set to Volume in Settings > Keys > KEYBOARD: its [ ] then step the RX volume in every window
 *  and mode, as its Volume Up and Down keys do, and it never keys the radio. A device is its bus, vendor and
 *  product, "0005:514c:8850" (kbd_dev_id); the ones set to Volume are the setting kbd_dev_volume, their ids
 *  a space between them. The rest is key_input.c's and paddle_keyer.c's.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KBD_DEV_ID_LEN   15                         /* "0005:514c:8850" and its NUL */
#define KBD_DEV_MAX      16                         /* devices set to Volume kept, the oldest dropped past it */
#define KBD_DEV_LIST_LEN (KBD_DEV_MAX * KBD_DEV_ID_LEN)

/* The id of the device with this bus, vendor and product */
void kbd_dev_id(char *out, unsigned bus, unsigned vendor, unsigned product);

/* The id is in the list */
bool kbd_dev_listed(const char *list, const char *id);

/* The list as stored: the well-formed ids only, in lower case, each once, the last KBD_DEV_MAX of them */
void kbd_dev_list_clean(const char *list, char *out, size_t size);

/* The list with the id put in (on, at the end) or taken out */
void kbd_dev_list_set(const char *list, const char *id, bool on, char *out, size_t size);

#ifdef __cplusplus
}
#endif
