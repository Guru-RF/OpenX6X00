/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#pragma once

/* Settings: eleven tabs on three button pages and a rail at the left */

#include "app_window.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

extern dialog_t *dialog_settings;

/* Before the settings are saved at power off: a Measure under way (TX Cal > S-METER) puts PRE, ATT and the RF gain back */
void settings_window_poweroff(void);

/* The radio's network name (Settings > Wireless > Network name): the web configurator is at http://<name>.local */
void settings_network_name(char *out, size_t size);

#ifdef __cplusplus
}
#endif
