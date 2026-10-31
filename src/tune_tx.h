/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - TUNE for an external tuner: the transmission (ext_tune.c asks for it, on a thread of its own).
 *
 *  A steady tone at TX_PLAYER_AUDIO_HZ through tx_player, the way the digital apps send theirs: in an app at its TX
 *  audio offset, so that it lands where the app transmits; elsewhere on the TX frequency itself (the dial, the other
 *  VFO in split, the TX shift's), with the base in USB-D for it and the mode put back after. Its own power and time
 *  limit; meanwhile the TX power stays at most the tune's (radio_set_tune) and the SWR protection stands aside for
 *  readings within the tune's power and time (tune_swr_limit, radio.c). The frequency, the mode, the VFOs, split and
 *  the settings are as they were after: tx_player moves the VFO and puts it back, as for the apps, and nothing here
 *  writes a setting. The rules are plain arithmetic; tools/tune tests them and the transmission on the PC.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "board.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TUNE_PWR_MIN_W  0.1f
/* The setting's top: the radio's PWR top (#87), 10 W on the X6100 and 8 W on the X6200, for a tuner that needs about
 * 9 W to match; 5 W on the X6200's battery through tune_plan's top_w. It was 5 W */
#define TUNE_PWR_MAX_W  BOARD_PWR_MAX
#define TUNE_ATU_MAX_W  1.0f    /* the most a tune sends with the radio's own ATU on (tune_atu_off) */
#define TUNE_TIME_MIN_S 5
#define TUNE_TIME_MAX_S 30
#define TUNE_GRACE_MS   1000    /* the SWR protection waits this long past the limit: the tune is unkeying */

/* Why the radio would not tune now, in the order they are told */
typedef enum {
    TUNE_GO = 0,
    TUNE_NO_TX,         /* it does not transmit on its TX frequency: the band, split, a TX shift */
    TUNE_ON_AIR,        /* it transmits already: PTT, an app, TCI, Winlink, CW, the ATU, the SWR scan */
    TUNE_WINLINK,       /* a Winlink connection is under way */
} tune_refusal_t;

typedef struct {
    bool tx_freq_ok;    /* cfg_is_tx_freq(radio_tx_freq()) */
    bool on_air;
    bool winlink;
} tune_ask_t;

tune_refusal_t tune_refusal(const tune_ask_t *a);

/* One tune, as it goes out */
typedef struct {
    int      tx_mode;   /* the base's mode for it (radio_mode_t), -1: the VFO's own (USB-D already) */
    int32_t  offset_hz; /* the tone from the TX frequency: the open app's TX audio offset, below it in LSB; 0 on it */
    float    pwr_w;
    uint32_t limit_ms;
} tune_plan_t;

/* mode: the VFO's (radio_mode_t); app_hz: the open app's TX audio offset, 0 none (in LSB and LSB-D the tone goes that
 * far below the dial, as the app's audio does); pwr_w and limit_s: the settings; top_w: the most the radio gives now
 * (its PWR top, lower on the X6200's battery) */
void tune_plan(tune_plan_t *p, int mode, int32_t app_hz, float pwr_w, float top_w, int limit_s);

/* A tune above TUNE_ATU_MAX_W goes out with the radio's own ATU off, on both radios (#87): the tuner at the antenna
 * matches it alone. ext_tune.c switches the ATU off before it keys, says so, and leaves it off */
bool tune_atu_off(const tune_plan_t *p);

/* A tune on the air, for the SWR protection (radio.c keeps it, radio_set_tune) */
typedef struct {
    float    pwr_w;     /* 0: none */
    uint64_t start_ms;
    uint32_t limit_ms;
} tune_guard_t;

/* The highest power reading that is the tune's own: the base doubles it for a moment at keying, it reads in 0.1 W */
#define TUNE_OWN_W(w)   (2.0f * (w) + 0.2f)

/* The SWR protection's limit for one reading during TX (swr_guard_check): off (0) while a tune is on, the reading is
 * the tune's own power and the tune is within its time limit (and TUNE_GRACE_MS); else the setting */
float tune_swr_limit(const tune_guard_t *t, float pwr_w, float setting, uint64_t now_ms);

/* How a tune ended */
typedef enum {
    TUNE_END_STOP,      /* stopped: its key or button again, the Panel, a change of frequency or mode */
    TUNE_END_LIMIT,     /* its time limit */
    TUNE_END_CUT,       /* the radio unkeyed it: the SWR protection, no audio, another program's unkey */
    TUNE_END_REFUSED,   /* never keyed: something else went on the air in between */
} tune_end_t;

/* Polled at each block of audio (about 46 ms): true ends the tune there, faded out */
typedef bool (*tune_stop_fn_t)(void *ctx);

/* The tune: keys, sends the tone until stop() or the limit, puts back what it changed. Blocks: its own thread */
tune_end_t tune_tx_run(const tune_plan_t *p, tune_stop_fn_t stop, void *ctx);

#ifdef __cplusplus
}
#endif
