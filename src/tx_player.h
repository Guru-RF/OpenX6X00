// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Xiegu X6100 LVGL GUI - digital-mode TX player (tx_player.c)
//
// Keys the radio and sends a waveform made by an app (FT8, JS8, PSK, Olivia, JTTY, WSPR,
// RTTY, SSTV, WEFAX), steering the audio level from the ALC and power the radio reads back.
// The waveform is made on TX_PLAYER_AUDIO_HZ: the VFO is moved for the length of the
// transmission so that it goes out at the offset the app asked for. Both calls block until
// the end: use them from a worker thread.
//
// Copyright (c) 2026 Giovanni Ricciardi IU1WXY, own code of the IU1WXY firmware

#ifndef X6100_TX_PLAYER_H
#define X6100_TX_PLAYER_H

#include <stddef.h>
#include <stdint.h>     /* int16_t, int32_t */
#include <stdbool.h>

#if defined(__cplusplus)
extern "C" {     /* the C++ apps include it too */
#endif /* extern "C" */

#define TX_PLAYER_AUDIO_HZ      1325    /* audio frequency the apps synthesise on */
#define TX_PLAYER_MAX_PWR_W     7.0f    /* power cap of the digital modes */

/* Polled once per audio block: true stops the transmission there */
typedef bool (*tx_abort_fn_t)(void *arg);

/* Gives the next audio: up to n samples into buf, returns how many, 0 at the end */
typedef size_t (*tx_fill_fn_t)(int16_t *buf, size_t n, void *ctx);

float tx_player_base_gain_offset(void);   /* dB to start with: base firmware and power set */

/* An app's own TX power for what it sends next (WSPR's, a TUNE's), in W; 0: the radio's setting, as for the other
 * apps. The calling thread's: set it on the thread that transmits, before tx_player_base_gain_offset and the
 * transmission, and 0 after. The radio's power setting is not changed and is put back after the transmission; the
 * level found is not kept for the others */
void tx_player_set_pwr(float w);

/* The player held by the calling thread from now until its tx_player_release, its tx_player_stream in between: for a
 * transmission that sets the radio up before it starts and puts it back after (a TUNE's power cap and mode, tune_tx.c),
 * so that no app's slot starts or keys meanwhile, as during the transmission itself. False: another transmission
 * holds it, nothing to release */
bool tx_player_claim(void);
void tx_player_release(void);

/* Send count samples (AUDIO_PLAY_RATE, not modified) at offset_hz. False when abort
 * stopped it before the end, or when it did not start: the radio on the air already, or another transmission
 * starting in the same moment */
bool tx_player_play(int16_t *samples, uint32_t count, int32_t offset_hz, float base_gain,
                    tx_abort_fn_t abort, void *abort_ctx);

/* The same with the audio asked block by block from fill, for transmissions too long to
 * keep in memory (WEFAX) */
bool tx_player_stream(tx_fill_fn_t fill, void *fill_ctx, int32_t offset_hz, float base_gain,
                      tx_abort_fn_t abort, void *abort_ctx);

#if defined(__cplusplus)
}
#endif /* __cplusplus */

#endif /* X6100_TX_PLAYER_H */
