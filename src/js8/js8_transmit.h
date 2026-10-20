/*
 *  SPDX-License-Identifier: GPL-3.0-only
 *
 *  Xiegu X6100 LVGL GUI - JS8 transmitter, C functions used by the dialog
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "js8_receive.h" /* JS8_RX_TEXT_LEN */
#include "js8_submode.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JS8_TX_ERR_LEN 96
/* Lowest TX offset in Hz; the highest comes from js8_speed_max_offset_hz(). */
#define JS8_TX_MIN_OFFSET 500

/* Idle; waiting for the slot at next_ms with a message queued; transmitting a frame. */
typedef enum { JS8_TX_IDLE, JS8_TX_WAITING, JS8_TX_KEYING } js8_tx_state_t;

typedef struct {
    js8_tx_state_t state; /* what it is doing */
    int frame; /* counted from 1 */
    int frames;
    int64_t next_ms; /* ms since the epoch, wall clock */
    float offset_hz;
    js8_speed_t speed;
    char text[JS8_RX_TEXT_LEN];
} js8_tx_status_t;

/* A message checked before sending. */
typedef struct {
    bool ok;
    int frames;
    float seconds; /* time on the air */
    char preview[JS8_RX_TEXT_LEN]; /* the text other stations will decode */
    char error[JS8_TX_ERR_LEN];
} js8_tx_preview_t;

/* Every callback is called on the transmitter thread. */
typedef struct {
    /* Transmit one frame: 16-bit samples at the rate passed to js8_tx_create,
     * peak at 80% of full scale, centred on synth_hz. Return when it has been
     * played, false if it was cut short. The samples may be overwritten. */
    bool (*play)(int16_t *pcm, unsigned count, int frame_index, int frame_count, void *ctx);
    void (*on_status)(const js8_tx_status_t *st, void *ctx);
    void (*on_done)(const char *message, bool completed, void *ctx);
    void *ctx; /* handed back to every callback */
} js8_tx_cb_t;

typedef struct js8_tx js8_tx_t; /* opaque */

void js8_tx_preview(const char *call, const char *grid, const char *message, js8_speed_t speed,
                    js8_tx_preview_t *result);
bool js8_tx_sendable_char(char ch);

/* rate is the output sample rate. Audio is made around synth_hz, or at the
 * offset asked for in js8_tx_send() when synth_hz is 0. */
js8_tx_t *js8_tx_create(int rate, float synth_hz, const js8_tx_cb_t *callbacks);

/* Starts sending message at offset_hz and the given speed. False, with the
 * reason in err, if it can't. */
bool js8_tx_send(js8_tx_t *t, const char *call, const char *grid, const char *message, float offset_hz,
                 js8_speed_t speed, char *err, unsigned err_size);

/* Drops the message being sent without waiting; js8_tx_busy() stays true
 * until play() returns for the current frame. Any thread may call it. */
void js8_tx_stop(js8_tx_t *t);
bool js8_tx_busy(js8_tx_t *t);
bool js8_tx_stopping(js8_tx_t *t);

/* Stops and joins the transmitter thread, then frees it. Accepts NULL. */
void js8_tx_destroy(js8_tx_t *t);

#ifdef __cplusplus
}
#endif
