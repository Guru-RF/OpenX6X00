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

/* Send count samples (AUDIO_PLAY_RATE, not modified) at offset_hz. False when abort
 * stopped it before the end */
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
