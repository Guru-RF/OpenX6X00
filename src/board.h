/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - what the radio's Linux shows the GUI: the devices by name, the panel's size.
 *  The X6100 unless the build says BOARD_X6200 (cmake -DHW_BACKEND=x6200). Another radio on
 *  the same code changes this file (and the backend behind hw/hw.h), not the places that use them.
 *
 *  Copyright (c) 2026 RF.Guru
 */

#pragma once

/* The panel, landscape */
#define BOARD_DISP_W        800
#define BOARD_DISP_H        480

/* The AF volume's range, the base's own: the X6200 takes 0..100 (gdyuldin's X6200 GUI, 0d504863), the X6100 0..55 */
#ifdef BOARD_X6200
#define BOARD_VOL_MAX       100
#else
#define BOARD_VOL_MAX       55
#endif

/* The CW sidetone level's range (KEY > Volume): the X6200's base takes 0..63, a 6-bit field, and
 * gdyuldin's X6200 GUI goes that far (#110); the X6100 stays at 0..32, as it always was */
#ifdef BOARD_X6200
#define BOARD_KEY_VOL_MAX   63
#else
#define BOARD_KEY_VOL_MAX   32
#endif

/* The top of the TX power setting (PWR), in W: the X6100 is a 10 W radio, the X6200 is rated 8 W on an
 * external supply, and gdyuldin's X6200 GUI stops its setting at 8.0 (vol.c). 10 W asked of the X6200
 * went to its base as 10 W (#53). On battery the X6200 stops at 5 W, as Xiegu's app (1.0.8) does
 * (radio.c follows the supply); the X6100 has no battery cap */
#ifdef BOARD_X6200
#define BOARD_PWR_MAX       8.0f
#define BOARD_PWR_MAX_BAT   5.0f
#else
#define BOARD_PWR_MAX       10.0f
#endif

#ifdef HOST_BUILD
/* The GUI on a PC (cmake -DHOST_BUILD=ON): an SDL window for the panel, the stub radio, and the
 * front keys and knobs as FIFOs under the data directory that host/host_panel.c writes the PC's
 * keys and mouse wheel into, in the devices' own event format (so the drivers and the browser
 * panel's key injection are the radio's). BOARD_DATA and BOARD_SHARE come from the build */
#define BOARD_DEV_KEYPAD    BOARD_DATA "/run/keypad"
#define BOARD_DEV_MAIN      BOARD_DATA "/run/main"
#define BOARD_DEV_VOL       BOARD_DATA "/run/vol"
#define BOARD_DEV_MFK       BOARD_DATA "/run/mfk"
#define BOARD_DEV_POWER     BOARD_DATA "/run/power"
#define BOARD_PA_SINK       NULL                    /* PulseAudio's defaults */
#define BOARD_PA_SOURCE     NULL
#define BOARD_MIXER_PLAY    NULL                    /* no codec levels to set */
#define BOARD_MIXER_REC     NULL
#define BOARD_CAT_DEV       BOARD_DATA "/run/cat"   /* none: X6100_CAT_DEV=<pty> gives one */
#define BOARD_RTC_DEV       "/dev/null"
#define BOARD_NAME          "host"
#define BOARD_NAME_LOWER    "x6100"
#else

/* The input devices (gui_main.c reads them, remote_server.c writes the browser's keys into them) */
#define BOARD_DEV_KEYPAD    "/dev/input/event0"     /* the front keys, a matrix keypad */
#define BOARD_DEV_MAIN      "/dev/input/event1"     /* the main knob */
#define BOARD_DEV_VOL       "/dev/input/event2"
#define BOARD_DEV_MFK       "/dev/input/event3"
#ifdef BOARD_X6200
#define BOARD_NAME          "X6200"
#define BOARD_NAME_LOWER    "x6200"                 /* the default network name, what WSJT-X's protocol is told */
#define BOARD_DEV_MFK_INNER "/dev/input/event4"     /* the inner ring of the MFK knob (not read yet) */
#define BOARD_DEV_POWER     NULL                    /* the base reports the POWER key in its packets */
#else
#define BOARD_NAME          "X6100"
#define BOARD_NAME_LOWER    "x6100"
#define BOARD_DEV_POWER     "/dev/input/event4"     /* the PMIC's key: the matrix keypad drops KEY_POWER */
#endif

/* PulseAudio's names for the codec, and its ALSA mixer elements for the play and record levels */
#define BOARD_PA_SINK       "alsa_output.platform-sound.stereo-fallback"
#define BOARD_PA_SOURCE     "alsa_input.platform-sound.stereo-fallback"
#define BOARD_MIXER_PLAY    "AIF1 DA0"
#define BOARD_MIXER_REC     "ADC"

/* The user's data: the settings database, logs, macros, recordings, themes. The radio's data
 * partition; a desktop build passes its own directory (-DBOARD_DATA='"..."') */
#ifndef BOARD_DATA
#define BOARD_DATA          "/mnt"
#endif

/* The CAT serial port behind the USB bridge, the clock chip */
#define BOARD_CAT_DEV       "/dev/ttyS2"
#define BOARD_RTC_DEV       "/dev/rtc1"
#endif

/* What the image ships read-only: the default settings, icons, the country list, the bundled
 * themes and pictures */
#ifndef BOARD_SHARE
#define BOARD_SHARE         "/usr/share/x6100"
#endif
