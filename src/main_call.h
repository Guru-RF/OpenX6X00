/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  The operator's callsign on the main screen (Settings > Display > MAIN SCREEN > Callsign, #292): what the layouts
 *  show, and a subject that changes with it. The callsign of Settings > General > STATION, a CB one too (it is the
 *  user's call all the same); nothing with the switch off or no callsign set. Each layout puts it where it has the
 *  room: beside the locator in the frequency strip (freq_strip.c), on the second row of the top bar in Black Metal
 *  and Total Black (info_bar.c), on the line under Amber Trey's display (lcd_panel.c).
 */

#pragma once

#include "cfg/subject_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Once, before the main screen is built: the switch followed, the callsign looked at every 2 s (Settings and the
 * first-start guide set it without a subject) */
void main_call_init(void);

/* The callsign to show, "" with the switch off or none set */
const char *main_call(void);

/* Set (to a new number) when main_call() changed: the layouts subscribe to it */
extern SubjectInt *main_call_subject;

#ifdef __cplusplus
}
#endif
