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

/* The panel, landscape. BOARD_FB_ROTATE_90 says whether the framebuffer is
 * portrait and needs a 90° blit (X6100/X6200: yes). A landscape framebuffer
 * sets it to 0; fbdev then copies without rotating. DRM still needs 1
 * (waterfall/Walkie planes); drm_init() falls back to fbdev when it is 0. */
#define BOARD_DISP_W        800
#define BOARD_DISP_H        480
#ifndef BOARD_FB_ROTATE_90
#define BOARD_FB_ROTATE_90  1
#endif

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

/* The rest of the radio's ranges. The same on the X6100 and the X6200; the settings manager reads them. */
#define BOARD_PWR_MIN       0.1f        /* PWR and the external tuner's TUNE power */
#define BOARD_SQL_MAX       100
#define BOARD_RFG_MAX       100
#define BOARD_RIT_MAX       1500
#define BOARD_XIT_MAX       1500
#define BOARD_MIC_MAX       50
#define BOARD_NB_MAX        100
#define BOARD_HF_MIN_HZ     100000      /* upstream f6c97fa */
#define BOARD_HF_MAX_HZ     55000000

/* Knob / encoder steps: the knob tables in radio_controls.cpp read these, and the PWR and TUNE power settings round
 * to BOARD_PWR_STEP. Same on the X6100 and the X6200 today. The settings keep PWR and TUNE power in tenths of a watt,
 * so BOARD_PWR_STEP is 0.1 or a multiple of it; the browser panel, Settings' TUNE power slider, the WLK/dig PWR
 * buttons and CAT have steps of their own. */
#define BOARD_PWR_STEP              0.1f
#define BOARD_PWR_LABEL_FMT         "%0.1f"
#define BOARD_SQL_STEP              1
#define BOARD_RFG_STEP              1
#define BOARD_RIT_STEP              10
#define BOARD_XIT_STEP              10
#define BOARD_NB_LEVEL_STEP         5
#define BOARD_NB_WIDTH_STEP         5
#define BOARD_NR_LEVEL_STEP         5
#define BOARD_MONI_STEP             1
#define BOARD_KEY_SPEED_STEP        1
#define BOARD_KEY_TONE_STEP         10
#define BOARD_KEY_VOL_STEP          1
#define BOARD_QSK_TIME_STEP         10
#define BOARD_KEY_RATIO_STEP        0.1f
#define BOARD_CW_DECODER_SNR_STEP   0.1f
#define BOARD_CW_PEAK_Q_STEP        1
#define BOARD_DNF_CENTER_STEP       50
#define BOARD_DNF_WIDTH_STEP        5
#define BOARD_WF_REF_STEP           1
#define BOARD_FILTER_LOW_STEP       10
#define BOARD_FILTER_BW_STEP        20

/* The low-battery power-off (bat_watch.c), the battery's voltage in 0.1 V: from there "Low battery! Turning off in
 * 30s.", then the charger set back On, the save and the radio off. The X6100 runs to 6.0 V, as it always did. The
 * X6200's base (1.0.8) cuts the power by itself at about 6.4-6.6 V, with nothing saved and the run's log lost: in
 * #160, on receive with WiFi and Bluetooth on, the first 6.8 V reading came 4-5 min before its cut, 6.7 V about
 * 2.5 min. At 6.8 V (v0.3.9) it went off in order on #160's radio and twice on the bench's, but the bench's behaves
 * erratically near the end of its battery, so 7.0 V: on receive about 3 min sooner (#160's run: 7.0 V at 00:14, 6.8 V
 * at 00:17), more for a TX at the end, which brings the cut nearer. A TX that sags the battery to 7.0 V starts it
 * too, and it is called off only at BAT_HIGH, 7.2 V: so with 7.1 V at rest a TX ends the run (#160's run: 7.2 V
 * half an hour before its power-off).
 * BOARD_BAT_EXT_MIN: on the X6200 only DC IN that the base reads at 12.0 V and more stops it (with hysteresis,
 * bat_watch.c), its manual's least for charging (12-13.8 V). Xiegu's 12 V 1 A charger reads 11.7-11.8 V, so it is
 * under. In #160 a charger at 11.6-11.8 V did not hold a flat battery up: with the radio on it fell from 6.9 to 6.4 V
 * in 2 minutes and the base cut the power again, its flags saying external power all the same (its charging flag
 * comes with any supply, radio.c). With the battery at 7.7-7.8 V the stock charger ran the radio (#33). On the X6100
 * any external power stops it, as before */
#ifdef BOARD_X6200
#define BOARD_BAT_LOW       70
#define BOARD_BAT_EXT_MIN   120
#else
#define BOARD_BAT_LOW       60
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
#define BOARD_DEV_MFK_INNER "/dev/input/event4"     /* the inner ring of the MFK knob: what the MFK controls */
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
