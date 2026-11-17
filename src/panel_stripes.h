/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - Panel stripes fix (Settings > Display > BACKLIGHT, #239): for panels that show vertical coloured stripes
 *  in the darkest shades, the display engine's gamma table sends 8..31 out as 32 in each colour (tcon_gamma.h)
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#pragma once

/* The setting followed from now on: put on the display engine now, again after the first frame's mode set, and
 * looked at every few seconds while on */
void panel_stripes_init(void);

/* Something may have reset the display engine (the panel lit again): the setting put on it again */
void panel_stripes_again(void);
