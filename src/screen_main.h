/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#pragma once

#include "lvgl/lvgl.h"
#include "params/params.h"

/* Memory slots: 1-9 the hand mic digits (long press), then the backup an app keeps the radio
 * in while it moves it; the digital apps have their own ranges */
#define MEM_HKEY_MAX_ID         9
#define MEM_BACKUP_ID   (MEM_HKEY_MAX_ID + 1)
#define MEM_FT8_ID      100
#define MEM_FT4_ID      200
#define MEM_WSPR_ID     300
#define MEM_SSTV_ID     400

void mem_load(uint16_t id);
void mem_save(uint16_t id);

/* Made once at start; the radio calls the notify functions */
lv_obj_t * main_screen();
void main_screen_notify_rx_tx(bool tx);
void main_screen_notify_low_power(bool is_low);

/* Another theme, live (Settings, the remote) */
void main_screen_set_theme(const char *name);

/* The keyboard to the spectrum; a dialog takes it away while open */
void main_screen_keys_enable(bool on);

/* Apps and the actions a long press or a hand mic F key can be given */
void main_screen_start_app(press_action_t app);
void main_screen_close_app(void);
void main_screen_action(press_action_t action);

/* The knobs' presses and the MFK (dir -1, +1 a turn, 0 the press), as on the main screen */
void main_screen_vol_press(void);
void main_screen_mfk(int dir);
void main_screen_agc_next();

/* Locks set by the apps, checked by the web remote. The frequency is locked while an app holds it or the user
 * locked it (main_screen_freq_locked); the app's hold leaves the user's lock as it was */
void main_screen_lock_freq(bool lock);
void main_screen_user_lock_freq(bool lock);
bool main_screen_freq_user_locked(void);
void main_screen_lock_band(bool lock);
void main_screen_lock_mode(bool lock);
void main_screen_lock_ab(bool lock);
bool main_screen_freq_locked();
bool main_screen_band_locked();
bool main_screen_mode_locked();
/* The digital app open (FT8, PSK31, Olivia, RTTY...), NULL for none: it holds the radio's frequency, mode and
 * audio. By the name it has now, as in the frequency strip: FT4 once the FT8 app was switched to it, PSK63. Any
 * thread (Winlink asks) */
const char *main_screen_digital_app(void);
/* The same as its action (ACTION_APP_FT8 ...), ACTION_NONE when none is open */
press_action_t main_screen_digital_app_action(void);

/* A jump to a frequency: the spectrum starts over */
void main_screen_set_freq(uint64_t freq);

/* One line of text in a box over the spectrum for a second ("CHANNEL" when text is empty) */
void channel_overlay_show(const char *text);
