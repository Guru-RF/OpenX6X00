/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - Olivia MFSK receiver, application layer
 *
 *  Path: the app's audio at 11025 Hz -> resampler to 8000 Hz ->
 *        olivia_rx -> decoded characters -> the Olivia window
 *
 *  Olivia needs a whole number of samples per symbol and a power-of-two
 *  FFT window, which 8000 Hz gives (256 and 1024 at 32/1000).
 *
 *  The protocol itself is in olivia_rx.c, which knows nothing of LVGL
 *  and can be tested on a host.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OLIVIA_IN_RATE 11025    /* AUDIO_CAPTURE_RATE / AUDIO_DECIM */
#define OLIVIA_RX_RATE 8000

#ifdef __cplusplus
extern "C" {
#endif

/* Receiving on and off. While off it costs nothing. */
void olivia_app_set_active(bool on);
bool olivia_app_is_active(void);

/* Audio at OLIVIA_IN_RATE, from the window's audio callback (and host tests). */
void olivia_app_put_audio_samples(size_t n, float *samples);

/*
 * Whether the lock is on the reversed tone order. The receiver checks
 * both orientations itself, because the operator cannot tell - the
 * spectrum looks IDENTICAL. For display only.
 */
bool olivia_app_reversed(void);

/* Re-read the variant and the centre frequency after the operator has
 * changed them. Drops the synchronisation, which no longer applies. */
void olivia_app_update_settings(void);

/* State for display. */
bool    olivia_app_locked(void);
float   olivia_app_freq_error(void);   /* Hz, positive = sender higher */
uint8_t olivia_app_quality(void);      /* 0..100 */

/* Diagnostics: they tell apart situations that look alike from outside
 * ("LOCK, but nothing shows"):
 *   blocks grow, chars 0  -> decoding zeros only (the sender is idle)
 *   blocks grow, chars >0 -> decoding, so the problem is in the display
 *   blocks do not grow    -> locked, but blocks do not complete */
uint32_t olivia_app_blocks(void);    /* decoded blocks since lock        */
uint32_t olivia_app_chars(void);     /* printable characters (no zeros)  */
float    olivia_app_raw_score(void); /* raw FHT figure, not a percentage */
float    olivia_app_level_db(void);  /* input audio level, dB            */
float    olivia_app_best_score(void);/* best hypothesis, also before lock */
float    olivia_app_best_offset_hz(void);/* where that hypothesis is, Hz */
uint8_t  olivia_app_clip_pct(void);  /* percentage of clipped samples    */

/*
 * Hooks for the Olivia window. Both are called from the audio thread
 * with the receiver's lock held, so they must be short and must not call
 * back into this module; hand the work to the UI thread instead.
 *
 * text_cb: decoded text goes here; a short NUL-terminated string per
 *          block, '\n' for a new line.
 * audio_tap: when set, sees every block of audio the receiver gets,
 *            while the receiver is active.
 *
 * After olivia_app_set_active(false) returns neither is called again.
 */
typedef void (*olivia_app_text_cb_t)(const char *text);
typedef void (*olivia_app_audio_tap_t)(unsigned int n, const float *samples);

void olivia_app_set_text_cb(olivia_app_text_cb_t cb);
void olivia_app_set_audio_tap(olivia_app_audio_tap_t tap);

#ifdef __cplusplus
}
#endif
