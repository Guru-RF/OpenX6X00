// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Xiegu X6100 LVGL GUI - digital-mode TX player (tx_player.h)
//
// Copyright (c) 2026 Giovanni Ricciardi IU1WXY, own code of the IU1WXY firmware

#include "tx_player.h"           /* tx_fill_fn_t, TX_PLAYER_* */

#include <math.h>                 /* log10f */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>               /* the 0 W line in the log */
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "lvgl/lvgl.h"           /* LV_MIN, LV_MAX */

#include "audio.h"               /* audio_play, audio_gain_db */
#include "cfg/cfg_api.h"         /* param_f_get */
#include "msg.h"                 /* the watchdog's and the 0 W notes */
#include "params/params.h"       /* the learned gain offset */
#include "radio.h"               /* radio_set_modem, radio_set_tx_freq */
#include "tx_info.h"             /* tx_info_refresh: ALC and power */
#include "tx_level.h"


/* Range of the gain applied to the samples, dB */
#define DRIVE_FLOOR_DB      (-42.0f)
#define DRIVE_TOP_DB        0.0f

/* The learned offset (kept between transmissions) never below this: an overdriven or long
 * transmission (SSTV) could push it to the floor, and the ramp up (8 dB a transmission at
 * most, tx_level.c) took many transmissions to bring the power back */
#define LEARNED_MIN_DB (-42.0f)

#define BLOCK_SAMPLES       2048    /* audio sent per pass, the ALC is read between passes */
#define STALL_MS            3000    /* keyed this long without a block of audio: unkeyed */

static atomic_bool     playing;
static atomic_bool     busy;       /* a transmission from its start to its end (tx_player_stream, tx_player_claim) */
static _Thread_local bool claimed; /* this thread's tx_player_claim, until its tx_player_release */
static atomic_uint_fast64_t last_block_ms;
/* tx_player_set_pwr: an app's own power, 0 none. The thread's own: a TUNE (tune_tx.c) and WSPR's beacon each set
 * theirs on their thread, and one never went out at the other's */
static _Thread_local float own_w;

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000;
}

/* A transmission whose audio stops coming (an app stuck, the playback stalled) would keep
 * the radio keyed on a dead carrier: the watchdog drops the modem bit, which ends it as
 * the SWR protection does */
static void *stall_watch(void *arg) {
    (void)arg;
    for (;;) {
        usleep(500000);
        if (atomic_load(&playing) && now_ms() - atomic_load(&last_block_ms) > STALL_MS && radio_get_modem()) {
            radio_set_modem(false);
            msg_schedule_text_fmt("TX stopped: no audio for %d s", STALL_MS / 1000);
        }
    }
    return NULL;
}

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

void tx_player_set_pwr(float w) {
    own_w = w;
}

bool tx_player_claim(void) {
    if (atomic_exchange(&busy, true)) return false;
    claimed = true;
    return true;
}

bool tx_player_busy(void) {
    return atomic_load(&busy);
}

void tx_player_release(void) {
    if (!claimed) return;
    claimed = false;
    atomic_store(&busy, false);
}

/* The power a transmission goes out at: the app's own, else the radio's setting at most the cap; then at most the
 * radio's top (a transverter's TX power limit, #104; the X6200 on battery): WSPR's own 5 W at 0.5 W there, and the
 * level control aims at what comes out. An own power has its own top: WSPR's 5 W, TUNE's the radio's PWR top for its
 * time limit (#87), which the cap held to 7 W */
static float tx_pwr(void) {
    return LV_MIN(own_w > 0.0f ? own_w : LV_MIN(param_f_get(cfg_pwr), TX_PLAYER_MAX_PWR_W), cfg_pwr_max_get());
}

bool tx_player_stream(tx_fill_fn_t fill, void *fill_ctx, int32_t offset_hz, float base_gain,
                      tx_abort_fn_t abort, void *abort_ctx) {
    bool claim = !claimed;      /* else the caller holds the player (tx_player_claim) and lets it go itself */

    /* One transmission at a time from its start: an app's slot and a TUNE (tune_tx.c) that start in the same moment
     * both found the radio in RX below, before either keyed, and played into one another */
    if (claim && atomic_exchange(&busy, true)) {
        fprintf(stderr, "tx_player: another transmission is under way, this one is not sent\n");
        return false;
    }
    /* The radio on the air already (a TCI session, the PTT): not a second transmission over it. With TCI audio
     * keyed in the DIG modes too (#17), an app's slot could start inside a TCI transmission: both waveforms went
     * out mixed, the TCI session ended under the app, and the app's audio went on into TCI's stream after it
     * was freed. Winlink checks this itself (winlink.c on_air) */
    if (radio_get_modem() || radio_get_state() != RADIO_RX) {
        if (claim) atomic_store(&busy, false);
        fprintf(stderr, "tx_player: the radio is on the air already, this transmission is not sent\n");
        return false;
    }

    float   pwr_w = tx_pwr();
    bool    own = own_w > 0.0f;
    /* The cap (RTTY and WeFax over 7 W) or an app's own power (WSPR's 0.2 W): the radio there for this transmission,
     * its setting back after */
    bool    pwr_set = pwr_w != param_f_get(cfg_pwr);

    if (pwr_set) radio_set_pwr(pwr_w);

    /* Part of the gain goes to the codec volume (6 dB above the total), the rest is applied
     * to the samples */
    float   total = base_gain + LV_MAX(params.ft8_output_gain_offset.x, LEARNED_MIN_DB);
    float   codec = audio_set_play_vol(total + 6.0f);
    float   drive = total - codec;

    /* The VFO that transmits moves for the tone: the other one in split (a TX shift), the IF shift put back after */
    int32_t dial = radio_tx_freq();

    radio_set_tx_freq(dial + offset_hz - TX_PLAYER_AUDIO_HZ);
    radio_set_modem(true);      /* keyed, the audio from the GUI */

    /* The drive from the ALC and power readback (tx_level.c): ramped up while the ALC reads
     * zero, held once it shows, kept for the next transmission */
    static uint8_t  reading;        /* the last tx_info reading seen */
    tx_level_t      level;

    tx_level_start(&level, pwr_w);
    level.power_hold = radio_caps() & HW_CAP_TX_NO_ALC;
    tx_info_refresh(&reading, NULL, NULL, NULL);    /* what came before this TX is not ours */

    /* Baseband r13 at 5 W (2026-10-12): the power holds at the setting from 0 dB down to -36 dB of
     * drive, while the ALC falls 9.9, 9.7, 9.3, 8.3, 7.0, 3.8, 0 (6 dB steps). The level used to be
     * learned no lower than -12 dB, where the ALC reads 9: the ALC rule now takes it down to where
     * the ALC just stops, about -36 dB, and the floors leave room for that. */
    int16_t block[BLOCK_SAMPLES];
    float   applied = drive;        /* gain on the previous block */
    bool    stopped = false;
    float   peak_w = 0.0f, peak_alc = 0.0f;    /* the highest readings after the settling, and how many */
    int     reads = 0;
    /* tx_level counts in blocks of BLOCK_SAMPLES (its 1.3 s to settle, its half-second windows), so it is given one
     * for each BLOCK_SAMPLES of audio, not one a fill. Winlink's fill gives 10 ms at a time: the settling was over
     * after 0.3 s, the base's overshoot at keying (ALC 5, twice the power) was taken for overdrive, and ARDOP frames
     * turned the level all the apps share down to the floor and kept it from climbing back (#66). A reading that
     * comes between two of its blocks waits for the next one, the last reading kept */
    size_t  played = 0;             /* samples played */
    float   pwr = 0.0f, alc = 0.0f; /* the last reading */
    bool    have = false;           /* not yet given to tx_level */

    static pthread_t watch;
    if (!watch && pthread_create(&watch, NULL, stall_watch, NULL) == 0) pthread_detach(watch);
    atomic_store(&last_block_ms, now_ms());
    atomic_store(&playing, true);

    for (;;) {
        bool    fresh = tx_info_refresh(&reading, &alc, &pwr, NULL);

        have = have || fresh;
        /* one more each time the audio has gone into a new block: with whole blocks each pass, the short last one
         * too, as before */
        if ((size_t)level.blocks <= (played + BLOCK_SAMPLES - 1) / BLOCK_SAMPLES) {
            drive = keep_in_range(drive + tx_level_block(&level, have, pwr, alc));
            have = false;
        }
        if (fresh && level.blocks > TX_LEVEL_SETTLE_BLOCKS) {
            peak_w = LV_MAX(peak_w, pwr);
            peak_alc = LV_MAX(peak_alc, alc);
            reads++;
        }

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
        played += n;
        atomic_store(&last_block_ms, now_ms());
    }
    atomic_store(&playing, false);
    tx_info_set_length(0);      /* the strip back to the time alone */

    /* Learned for the next transmission of any app: they share the audio path. With no ALC to go by, a level that
     * was found; one still climbing to the setting only back up towards the default (0), where the X6200 gives 5 W
     * at once. Without that, a level knocked down once (an ALC or power reading over: a mag loop not yet tuned)
     * climbed its 8 dB a transmission, never reached the setting, was never saved, and FT8 stayed at 0 W (#16) */
    float level_db = drive - base_gain + codec;
    float learned = LV_MAX(level_db, LEARNED_MIN_DB);

    /* No power read at all once the PA settled, a whole transmission long (#16): said on the screen and in the log
     * with what makes the level, so that a report tells the learned level, the Line-in and the base apart. From a
     * 0.2 W setting only: the reading is whole tenths, cut off, so at 0.1 W (FT8's lowest step, WSPR's 20 dBm) a
     * true 0.09 W reads 0.0 a whole normal transmission long, and the X6200 lost its level each time */
    bool no_rf = !stopped && reads > 0 && level.blocks > TX_LEVEL_SETTLE_BLOCKS + 2 * TX_LEVEL_WINDOW_BLOCKS
              && peak_w < TX_LEVEL_PWR_STEP_W && level.target_w > 1.5f * TX_LEVEL_PWR_STEP_W;

    if (no_rf) {
        fprintf(stderr, "tx_player: 0 W the whole TX: level %.1f dB (codec %.1f, samples %.1f), line_in %u, "
                "ALC %.1f, PWR %.1f W%s\n", level_db, codec, drive, params.line_in, peak_alc, level.target_w,
                level.power_hold && !own ? ", the level back to 0 dB" : "");
        msg_schedule_text_fmt("TX 0 W: level %.0f dB, Line-in %u", level_db, params.line_in);
    }
    if (own) {
        /* An app's own power (a 0.2 W WSPR beacon): what it found is not kept, it would pull the level the other apps
         * start from (FT8 at 5 W) down with it */
    } else if (level.power_hold && no_rf) {
        /* Nothing came out: the next transmission from the default, where the X6200 gives the setting at once, not
         * 8 dB a transmission up from a level near the floor */
        params_float_set(&params.ft8_output_gain_offset, 0.0f);
    } else if (!level.power_hold || level.held) {
        params_float_set(&params.ft8_output_gain_offset, learned);
    } else if (params.ft8_output_gain_offset.x < 0.0f && learned > params.ft8_output_gain_offset.x) {
        params_float_set(&params.ft8_output_gain_offset, LV_MIN(learned, 0.0f));
    }
    audio_play_wait();          /* the last block out before unkeying */
    radio_set_modem(false);     /* unkeyed: back to the dial frequency */
    radio_set_tx_freq(0);
    audio_set_play_vol(params.play_gain_db_f.x);   /* the playback volume of the settings */
    if (pwr_set) {
        /* the setting back: RTTY and WeFax left the radio at the cap, also in SSB after */
        radio_set_pwr(param_f_get(cfg_pwr));
    }
    if (claim) atomic_store(&busy, false);
    return !stopped;
}

float tx_player_base_gain_offset(void) {
    /* A base that sets the power itself (the X6100 from rev 3 on, the X6200, whose stock GUI
     * plays at one level too) takes the audio at one level; the OEM X6100 base scales it, to the
     * power this transmission goes out at */
    if (radio_caps() & HW_CAP_TX_LEVEL) return -9.4f;

    return -16.4f + 10.0f * log10f(tx_pwr());
}
