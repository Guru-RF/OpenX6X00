/*
 *  SPDX-License-Identifier: GPL-3.0-only
 *
 *  Xiegu X6100 LVGL GUI - JS8 receive for C code (the JS8 window)
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JS8_RX_CALL_LEN 16 /* callsign buffers, NUL included */
/* Room for a whole message: desktop sends up to 20 frames, about 425
 * characters, and "CALL: " goes in front. 256 used to cut long ones. */
#define JS8_RX_TEXT_LEN 640 /* NUL included */

/* Speeds to decode, for js8_rx_create() and js8_rx_set_submodes(). */
#define JS8_SUBMODE_NORMAL 0x1
#define JS8_SUBMODE_FAST   0x2
#define JS8_SUBMODE_TURBO  0x4
#define JS8_SUBMODE_SLOW   0x8

/* js8_rx_msg_t.type bits. */
#define JS8_FRAME_FIRST (1 << 0)
#define JS8_FRAME_LAST  (1 << 1)
#define JS8_FRAME_DATA  (1 << 2)

/* A decoded frame or a whole message. No pointers inside, so it can be
 * copied byte for byte (scheduler_put() does). */
typedef struct {
    int32_t  utc;            /* time of day, HHMMSS */
    int16_t  snr;            /* dB */
    float    dt;             /* seconds off the slot start */
    int32_t  drift_ms;       /* the JS8 drift that puts this station on time: the drift
                                in use when its audio came in, minus its DT, so it holds
                                even if the drift has changed since */
    float    freq_hz;        /* audio offset */
    uint8_t  type;           /* JS8_FRAME_* of the frame, the last one for a message */
    uint8_t  submode;        /* speed by desktop number: 0 Normal, 1 Fast, 2 Turbo, 4 Slow */
    bool     low_confidence; /* shown in [brackets] by desktop */
    bool     heartbeat;      /* @HB HEARTBEAT */
    bool     snr_report;     /* "CALL SNR -12", usually a heartbeat answered */
    bool     cq;             /* CQ or @ALLCALL CQ */
    bool     to_me;          /* sent to our callsign */
    bool     to_group;       /* sent to an @GROUP */
    int8_t   checksum;       /* buffered commands (MSG...): 1 good, -1 wrong, 0 none */
    bool     tx;             /* the app sets it on what it sent itself */
    bool     alert;          /* the app sets it when an alert word matched */
    bool     partial;        /* on_message only: text so far of an unfinished message */
    uint32_t msg_id;         /* same for a message's partial texts and the message */
    char     from[JS8_RX_CALL_LEN]; /* sender, upper case, "" when unknown */
    char     to[JS8_RX_CALL_LEN];   /* callsign or @GROUP it is sent to, or "" */
    char     text[JS8_RX_TEXT_LEN]; /* as shown, cut to fit */
} js8_rx_msg_t;

/* Called from receiver threads, never from the one that created it. */
typedef struct {
    /* Each decoded frame. */
    void (*on_frame)(const js8_rx_msg_t *frame, void *user);
    /* Each whole message. Before it, with partial set, the text so far after
     * every frame of a message with more than one; the final message then has
     * the same msg_id. */
    void (*on_message)(const js8_rx_msg_t *message, void *user);
    void (*on_cycle_done)(unsigned decoded, void *user);
    /* Audio at the input rate, off the audio thread, for a waterfall. */
    void (*on_audio)(const float *audio, unsigned count, void *user);
    void *ctx; /* passed to every callback as `user` */
} js8_rx_cb_t;

typedef struct js8_rx js8_rx_t; /* opaque */

/* NULL if it can't be started. `my_call` can be NULL or "". */
js8_rx_t *js8_rx_create(int rate, int submode_mask, const char *my_call, const js8_rx_cb_t *callbacks);

/* Queue `count` float samples (-1..1) at the rate given to create. Fine to
 * call from the audio thread. */
void js8_rx_feed(js8_rx_t *receiver, const float *audio, unsigned count);

/* Forget messages still coming in, e.g. when the band changes. */
void js8_rx_clear(js8_rx_t *receiver);
/* Which speeds to decode (JS8_SUBMODE_* bits). Any thread. */
void js8_rx_set_submodes(js8_rx_t *receiver, int submode_mask);
/* The audio range searched for signals, and our TX offset, around which
 * decoding starts. Any thread. */
void js8_rx_set_decode_range(js8_rx_t *receiver, int from_hz, int to_hz);
void js8_rx_set_qso_offset(js8_rx_t *receiver, int hz);

/* Test mode: a 16-bit PCM WAV at any rate (resampled when needed) goes into
 * the decoder in real time from the next 30 s boundary, which starts a slot
 * at every speed, so a file that begins on a slot decodes like the air.
 * js8_rx_feed() audio is dropped meanwhile. Returns the seconds to the start,
 * or -1 with the reason in err. */
float js8_rx_play_wav(js8_rx_t *receiver, const char *file, char *err, unsigned err_size);
void  js8_rx_stop_wav(js8_rx_t *receiver);
bool  js8_rx_wav_active(js8_rx_t *receiver);

/* Stops its threads; once it returns, no callback runs. Takes NULL. */
void js8_rx_destroy(js8_rx_t *receiver);

/* JS8 time works like desktop JS8Call's time drift: the system clock plus a
 * drift in ms set by Time Sync, used for receive windows and transmit slots
 * while the system clock stays untouched. One drift for the whole process,
 * 0 at start, kept until the radio restarts. */
int64_t js8_wall_ms(void);           /* JS8 time, ms since 1970 */
void    js8_set_drift_ms(int64_t drift); /* from Time Sync */
int64_t js8_drift_ms(void);          /* the drift now in use */

#ifdef __cplusplus
}
#endif
