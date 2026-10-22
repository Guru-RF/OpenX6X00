/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - what the X6100's Linux shows the GUI: the devices by name, the panel's size.
 *  Another radio on the same code changes this file (and the control library behind radio.c),
 *  not the places that use them.
 *
 *  Copyright (c) 2026 RF.Guru
 */

#pragma once

/* The panel, landscape */
#define BOARD_DISP_W        800
#define BOARD_DISP_H        480

/* The input devices (gui_main.c reads them, remote_server.c writes the browser's keys into them) */
#define BOARD_DEV_KEYPAD    "/dev/input/event0"     /* the front keys, a matrix keypad */
#define BOARD_DEV_MAIN      "/dev/input/event1"     /* the main knob */
#define BOARD_DEV_VOL       "/dev/input/event2"
#define BOARD_DEV_MFK       "/dev/input/event3"
#define BOARD_DEV_POWER     "/dev/input/event4"     /* the PMIC's key: the matrix keypad drops KEY_POWER */

/* PulseAudio's names for the codec */
#define BOARD_PA_SINK       "alsa_output.platform-sound.stereo-fallback"
#define BOARD_PA_SOURCE     "alsa_input.platform-sound.stereo-fallback"

/* The user's data: the settings database, logs, macros, recordings, themes. The X6100's data
 * partition; a desktop build passes its own directory (-DBOARD_DATA='"..."') */
#ifndef BOARD_DATA
#define BOARD_DATA          "/mnt"
#endif

/* The CAT serial port behind the USB bridge, the clock chip */
#define BOARD_CAT_DEV       "/dev/ttyS2"
#define BOARD_RTC_DEV       "/dev/rtc1"
