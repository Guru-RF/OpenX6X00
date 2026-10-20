// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Xiegu X6100 LVGL GUI - WeFax receiver: FM discriminator, line builder and
// the Auto logic (APT start/stop tones, black band, phasing pulse).
//
// Input: mono float audio at WEFAX_SAMPLE_RATE, USB with the fax centred on
// WEFAX_CENTER_HZ. Output: one grey row of WEFAX_WIDTH pixels every line
// (120 LPM, IOC 576), through the row callback, in the audio thread.

#ifndef X6100_WEFAX_DECODER_H
#define X6100_WEFAX_DECODER_H

#include <stdbool.h>     /* the auto_* answers */
#include <stdint.h>      /* uint8_t rows */

#define WEFAX_SAMPLE_RATE   11025       /* Hz, the audio this takes */
#define WEFAX_CENTER_HZ     1500        /* audio Hz between black and white */
#define WEFAX_SHIFT_HZ      850         /* Hz from black to white */
#define WEFAX_LPM           120         /* lines per minute */
#define WEFAX_IOC           576         /* index of cooperation */
#define WEFAX_WIDTH         1809        /* IOC * pi */

enum wefax_mode {
    WEFAX_MODE_IDLE = 0,    /* no rows */
    WEFAX_MODE_CONT,        /* rows from the first sample */
    WEFAX_MODE_AUTO         /* rows after an APT start, kept once the black band confirms it */
};
typedef enum wefax_mode wefax_mode_t;

typedef struct wefax_decoder wefax_decoder_t;   /* opaque, see decoder.c */

typedef void (*wefax_row_callback_t)(const uint8_t *row, unsigned int width, unsigned int row_number, void *user_data);

wefax_decoder_t *wefax_decoder_create(void);    /* NULL without memory */
void wefax_decoder_destroy(wefax_decoder_t *decoder);

/* Back to the start of the mode. After a STOP in Auto, start detection pauses a few seconds */
void wefax_decoder_reset(wefax_decoder_t *decoder);

/* Set the mode and reset */
void wefax_decoder_set_mode(wefax_decoder_t *decoder, wefax_mode_t mode);
wefax_mode_t wefax_decoder_get_mode(const wefax_decoder_t *decoder);

void wefax_decoder_set_row_callback(wefax_decoder_t *decoder, wefax_row_callback_t callback, void *user_data);
void wefax_decoder_process(wefax_decoder_t *decoder, const float *samples, unsigned int count);

/* Auto: the stop tone was heard (the fax is over, save and re-arm with set_mode) */
bool wefax_decoder_auto_stop_detected(const wefax_decoder_t *decoder);

/* Auto: counts the start tones taken; a change means a new fax began */
uint32_t wefax_decoder_auto_start_generation(const wefax_decoder_t *decoder);

/* Auto: the black band after the start tone was seen, the rows are a real fax */
bool wefax_decoder_auto_fax_confirmed(const wefax_decoder_t *decoder);

#endif /* X6100_WEFAX_DECODER_H */
