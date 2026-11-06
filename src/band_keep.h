/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  The bands an app tunes across, left as it found them: the Scanner and the Propagation beacons. A tune into a band
 *  stores the app's frequency and mode in it (the band's memory, as a band key or a tune by hand does), so after a
 *  beacon sweep the 20 m key went to 14.100 in CW, and after a Channels scan each band to its last channel in that
 *  channel's mode. The radio itself goes back on close (dialog_t.restore_radio, MEM_BACKUP_ID) and so does the band
 *  it was on; the others did not.
 *
 *  The app calls band_keep_tune() before each retune: the first time into a band, its dial is taken as the settings
 *  file has it (cfg_band_dial_get), the band in use too (a band key went there while the app was open).
 *  band_keep_end(), from its destruct_cb, takes the radio back first (the backup memory, which app_window then loads
 *  again: nothing changes) and puts each band's dial back. A power-off with the app open calls it before the settings
 *  are flushed (screen_main.c), in receive only: it retunes, so on the air the bands stay as the app left them. A second
 *  call, or one with no app open, does nothing.
 */
#pragma once

#include <stdint.h>

void band_keep_begin(void);
void band_keep_tune(int32_t freq);
void band_keep_end(void);
