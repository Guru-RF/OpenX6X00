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
/* Something in CW takes that audio now: the decoder, the tuning bars or the CW trainer. Any thread */
bool cw_audio_wanted(void);

/* Received tone (Hz), smoothed: what the tuning indicator shows */
float cw_get_tone_freq(void);

/* Decoded speed (WPM), the last one measured */
int cw_get_wpm(void);

/* CW trainer: decode always (the decoder listens to the sidetone), its speed down to 5 WPM, the lowest key speed (10
 * on the band) */
void cw_trainer_set(bool on);
/* The CW trainer is open (cw_trainer_set): any thread */
bool cw_trainer_on(void);
/* The CW trainer keys a group: the next `elements` dits and dahs the decoder hears are its own, and the characters
 * they make come to the trainer marked own (cw_decoder_set_sink), however late the decoder ends the last one. 0: no
 * more of them (the characters started keep their mark). Any thread */
void cw_trainer_own(int elements);
/* The CW trainer keys at wpm: the decoder takes that speed at once when its own is more than 25% off (as the
 * trainer opens or the speed changes), else it goes on learning it. Any thread */
void cw_trainer_speed(int wpm);

#ifdef __cplusplus
}
#endif
