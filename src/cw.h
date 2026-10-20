/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  CW receive: the decoder and the tuning indicator fed from the RX audio.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void cw_init(void);

/* Audio thread: RX audio after the app decimation (AUDIO_CAPTURE_RATE / AUDIO_DECIM) */
void cw_put_audio_samples(unsigned int n, float *samples);

/* Received tone (Hz), smoothed: what the tuning indicator shows */
float cw_get_tone_freq(void);

/* Decoded speed (WPM), the last one measured */
int cw_get_wpm(void);

/* CW trainer: decode always (the decoder listens to the sidetone) */
void cw_trainer_set(bool on);

#ifdef __cplusplus
}
#endif
