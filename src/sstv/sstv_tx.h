/*
 * SSTV transmitter: a PNG (stretched to the mode's size) to 16-bit mono
 * audio, VIS header first, at any rate from 8 kHz up.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "sstv_mode.h"

/* *pcm is malloc'd (free() it).  0, or < 0: -20 bad arguments or a mode we
 * do not send, -10 no memory, other values: the PNG could not be read. */
int sstv_tx_pcm(sstv_mode_t mode, const char *png, unsigned rate, int16_t **pcm, size_t *count);
