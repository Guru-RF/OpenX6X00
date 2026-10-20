// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Xiegu X6100 LVGL GUI - digital-mode TX player (tx_player.h)
//
// Copyright (c) 2026 Giovanni Ricciardi IU1WXY, own code of the IU1WXY firmware

#include "tx_player.h"           /* tx_fill_fn_t, TX_PLAYER_* */

#include <math.h>                 /* log10f */
#include <string.h>

#include "lvgl/lvgl.h"           /* LV_MIN, LV_MAX */

#include "audio.h"               /* audio_play, audio_gain_db */
#include "cfg/cfg_api.h"         /* cparam_i_get, param_f_get */
#include "params/params.h"       /* the learned gain offset */
#include "radio.h"               /* radio_set_modem, radio_set_freq */
#include "tx_info.h"             /* tx_info_refresh: ALC and power */
#include "tx_level.h"

#include <aether_radio/x6100_control/control.h>   /* x6100_control_get_base_ver */

/* Range of the gain applied to the samples, dB */
#define DRIVE_FLOOR_DB      (-42.0f)
#define DRIVE_TOP_DB        0.0f

/* The learned offset (kept between transmissions) never below this: an overdriven or long
 * transmission (SSTV) could push it to the floor, and the ramp up (8 dB a transmission at
 * most, tx_level.c) took many transmissions to bring the power back */
#define LEARNED_MIN_DB (-42.0f)

#define BLOCK_SAMPLES       2048    /* audio sent per pass, the ALC is read between passes */

static float keep_in_range(float db) {
    if (db > DRIVE_TOP_DB) return DRIVE_TOP_DB;
    return db < DRIVE_FLOOR_DB ? DRIVE_FLOOR_DB : db;
}

/* tx_player_play's samples handed out as a stream */
typedef struct {
    const int16_t   *next;
    uint32_t        left;
} samples_t;

static size_t take_samples(int16_t *buf, size_t n, void *arg) {
    samples_t *s = arg;

    if (n > s->left) n = s->left;
    memcpy(buf, s->next, n * sizeof(int16_t));
    s->next += n;
    s->left -= n;
    return n;
}

bool tx_player_play(int16_t *samples, uint32_t count, int32_t offset_hz, float base_gain,
                    tx_abort_fn_t abort, void *abort_ctx) {
    samples_t s = { .next = samples, .left = count };

    return tx_player_stream(take_samples, &s, offset_hz, base_gain, abort, abort_ctx);
}

bool tx_player_stream(tx_fill_fn_t fill, void *fill_ctx, int32_t offset_hz, float base_gain,
                      tx_abort_fn_t abort, void *abort_ctx) {
    bool    over_cap = param_f_get(cfg_pwr) > TX_PLAYER_MAX_PWR_W;

    if (over_cap) radio_set_pwr(TX_PLAYER_MAX_PWR_W);

    /* Part of the gain goes to the codec volume (6 dB above the total), the rest is applied
     * to the samples */
    float   total = base_gain + LV_MAX(params.ft8_output_gain_offset.x, LEARNED_MIN_DB);
    float   codec = audio_set_play_vol(total + 6.0f);
    float   drive = total - codec;

    int32_t dial = cparam_i_get(cfg_fg_freq);

    radio_set_freq(dial + offset_hz - TX_PLAYER_AUDIO_HZ);
    radio_set_modem(true);      /* keyed, the audio from the GUI */

    /* The drive from the ALC and power readback (tx_level.c): ramped up while the ALC reads
     * zero, held once it shows, kept for the next transmission */
    static uint8_t  reading;        /* the last tx_info reading seen */
    tx_level_t      level;

    tx_level_start(&level, LV_MIN(param_f_get(cfg_pwr), TX_PLAYER_MAX_PWR_W));
    tx_info_refresh(&reading, NULL, NULL, NULL);    /* what came before this TX is not ours */

    /* Baseband r13 at 5 W (2026-10-12): the power holds at the setting from 0 dB down to -36 dB of
     * drive, while the ALC falls 9.9, 9.7, 9.3, 8.3, 7.0, 3.8, 0 (6 dB steps). The level used to be
     * learned no lower than -12 dB, where the ALC reads 9: the ALC rule now takes it down to where
     * the ALC just stops, about -36 dB, and the floors leave room for that. */
    int16_t block[BLOCK_SAMPLES];
    float   applied = drive;        /* gain on the previous block */
    bool    stopped = false;

    for (;;) {
        float   pwr = 0.0f, alc = 0.0f;
        bool    fresh = tx_info_refresh(&reading, &alc, &pwr, NULL);

        drive = keep_in_range(drive + tx_level_block(&level, fresh, pwr, alc));

        size_t n = fill(block, BLOCK_SAMPLES, fill_ctx);

        if (n == 0) break;
        /* the app's stop, or the modem dropped by the SWR protection (radio.c) */
        stopped = (abort && abort(abort_ctx)) || !radio_get_modem();
        if (stopped) break;
        if (drive != applied) {
            audio_gain_db_transition(block, n, applied, drive, block);   /* a ramp, no click */
            applied = drive;
        } else if (drive != 0.0f) {
            audio_gain_db(block, n, drive, block);
        }
        audio_play(block, n);
    }

    /* Learned for the next transmission of any app: they share the audio path */
    params_float_set(&params.ft8_output_gain_offset, LV_MAX(drive - base_gain + codec, LEARNED_MIN_DB));
    audio_play_wait();          /* the last block out before unkeying */
    radio_set_modem(false);     /* unkeyed: back to the dial frequency */
    radio_set_freq(dial);
    audio_set_play_vol(params.play_gain_db_f.x);   /* the playback volume of the settings */
    if (over_cap) {
        radio_set_pwr(param_f_get(cfg_pwr));   /* RTTY and WeFax left the radio at the cap, also in SSB after */
    }
    return !stopped;
}

float tx_player_base_gain_offset(void) {
    /* Bases from rev 3 on (patched firmware) set the power themselves */
    if (x6100_control_get_base_ver().rev >= 3) return -9.4f;

    return -16.4f + 10.0f * log10f(LV_MIN(param_f_get(cfg_pwr), TX_PLAYER_MAX_PWR_W));
}
