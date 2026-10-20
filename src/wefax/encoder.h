#pragma once

#include <stddef.h>
#include <stdint.h>

#include "decoder.h"

/*
 * WeFax transmitter, the decoder's format (IOC 576, 120 LPM, 1809 px).
 *
 *   APT start  300 Hz black/white, 5 s
 *   phasing    60 lines black with a 5% white pulse at the line start
 *   picture    grey, 0.5 s a line
 *   APT stop   450 Hz black/white, 5 s
 *   black      10 s
 *
 * FM with continuous phase, WEFAX_CENTER_HZ +/- WEFAX_SHIFT_HZ / 2 (black low,
 * white high). The audio is generated block by block, so a long fax needs no
 * big buffer.
 */

#define WEFAX_TX_MAX_ROWS 800

typedef struct wefax_encoder wefax_encoder_t;

/* Load a PNG, fit it to 1809 x at most 800 (aspect kept, white margins) in
 * grey. NULL on error, *rc < 0 then. */
wefax_encoder_t *wefax_encoder_open(const char *png_path, unsigned sample_rate, int *rc);

void wefax_encoder_close(wefax_encoder_t *enc);

/* Up to n samples into buf, 0 at the end (as tx_fill_fn_t, ctx is the encoder) */
size_t wefax_encoder_fill(int16_t *buf, size_t n, void *enc);

/* Picture lines, and progress 0..1000 */
unsigned wefax_encoder_rows(const wefax_encoder_t *enc);
int wefax_encoder_progress(const wefax_encoder_t *enc);
