/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - RTTY's transmitter (#312): text into ITA2 codes, the codes into AFSK. No radio and no LVGL here:
 *  rtty.c queues the text and hands rtty_tx_fill to tx_player_stream, the TX path of the other digital apps;
 *  tools/rtty_tx tests it on the PC.
 *
 *  A transmission: RTTY_TX_IDLE_S of mark, RTTY_TX_DIDDLES LTRS (the diddle a receiver finds the characters' start
 *  bits on), CR LF so that the text starts on a line of its own at the other end, the text, RTTY_TX_TAIL_BITS of
 *  mark, faded out. A character is 1 start bit (space), 5 data bits, the first one sent in bit 0 of the code
 *  (1: mark), and 1.5 stop bits (mark). The phase runs on from one tone into the other, and the bit edges fall on
 *  the fractional sample, so 45.45 Bd stays 45.45 Bd over a long line.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The ITA2 codes that are no character of either case */
#define RTTY_ITA2_LF        0x02
#define RTTY_ITA2_SPACE     0x04
#define RTTY_ITA2_CR        0x08
#define RTTY_ITA2_FIGS      0x1B
#define RTTY_ITA2_LTRS      0x1F

#define RTTY_TX_IDLE_S      0.5f    /* mark before the first character */
#define RTTY_TX_DIDDLES     3       /* LTRS after it */
#define RTTY_TX_STOP_BITS   1.5f
#define RTTY_TX_TAIL_BITS   3.0f    /* mark after the last character, the fade out at its end */
#define RTTY_TX_AMP         (32767.0f * 0.8f)   /* tx_player.c: the apps' waveforms peak at 0.8 of full scale */
#define RTTY_TX_RAMP_S      0.005f  /* the fade in at the start and the fade out at the end: no click */

/* The characters of each code, letters and figures (the US teletype's figures, as most RTTY programs print them),
 * the code's first bit in bit 0. '\n' is LF; 0 is none of them: NUL, CR, the bell, and the two shifts. The
 * decoder (rtty.c) prints with them, the encoder below sends with them: one table for both */
extern const char rtty_ita2_letters[32];
extern const char rtty_ita2_figures[32];

/* The mark on the higher of the two audio tones. side: +1 USB or USB-D, -1 LSB or LSB-D, 0 another mode. With Reverse
 * off the mark is the higher radio frequency in either sideband, as amateur RTTY sends it: in USB the higher tone, in
 * LSB the lower. The receiver takes mark and space by this rule, and the transmitter sends by it: what decodes right
 * goes out right */
bool rtty_mark_high(int side, bool reverse);

/* The two tones, shift apart around the centre */
void rtty_tones(uint16_t center, uint16_t shift, bool mark_high, float *mark_hz, float *space_hz);

/* The Preset button's (RTTY 5:5): rate, shift and Reverse at once, for the decoder and the transmitter. Weather is
 * the app's defaults (params.c), the weather stations' 50 baud and 450 Hz; Ham the usual amateur RTTY */
typedef enum {
    RTTY_PRESET_WEATHER,
    RTTY_PRESET_HAM,
    RTTY_PRESETS
} rtty_preset_id_t;

typedef struct {
    const char *name;           /* English, translated where it is shown */
    uint32_t    rate;           /* in 1/100 baud, as params.rtty_rate */
    uint16_t    shift;          /* Hz */
    bool        reverse;
} rtty_preset_t;

extern const rtty_preset_t rtty_presets[RTTY_PRESETS];

/* The preset these settings are, -1 none */
int rtty_preset_find(uint32_t rate, uint16_t shift, bool reverse);

/* The preset a press gives after current (rtty_preset_find's): Weather after Ham, Ham after any other, so that one
 * press always brings ham RTTY and a second the weather stations' back */
int rtty_preset_next(int current);

/* The case the far receiver is in, as far as the sender can know: LTRS, FIGS, or 0 when it can't. After a space sent
 * in figures it can't: a receiver that unshifts on space (USOS, the usual default) is back in letters, one that
 * doesn't is still in figures; the next figure or letter gets its shift again, and both print it right */
typedef struct {
    uint8_t shift;
} rtty_enc_t;

/* In letters: a transmission starts with LTRS (the diddles) */
void rtty_enc_init(rtty_enc_t *e);

/* The codes of a character, up to 3, into codes: its shift first when the receiver may be in the other case, CR LF
 * for '\n'. Lower case goes as capitals, a tab as a space, '\r' as nothing ('\n' brings CR). 0: ITA2 has no such
 * character, it is left out */
int rtty_enc_char(rtty_enc_t *e, char c, uint8_t *codes);

/* What the other end prints for c, as rtty_enc_char sends it; 0 when nothing (a character left out) */
char rtty_enc_shown(char c);

/* The AFSK: tones of whole or fractional bits in a queue, made into samples as they are asked for */
#define RTTY_AFSK_SEGS      64

typedef struct {
    float    spb;               /* samples a bit */
    float    dphi[2];           /* phase step a sample: space [0], mark [1] */
    float    phase;
    double   pos;               /* where the tones queued so far end, in samples from the start */
    uint32_t n;                 /* samples made */
    uint32_t ramp;              /* samples of a fade */
    uint32_t fade_end;          /* the sample the fade out ends on, 0: none asked */
    struct {
        uint8_t  mark;
        uint32_t end;           /* the sample this tone ends before */
    } seg[RTTY_AFSK_SEGS];
    int      head, count;
} rtty_afsk_t;

void rtty_afsk_init(rtty_afsk_t *a, float rate, float baud, float mark_hz, float space_hz);
/* A tone bits long after the ones queued; false: the queue is full */
bool rtty_afsk_tone(rtty_afsk_t *a, bool mark, float bits);
/* A character's frame: start bit, the 5 bits of code, the stop bits */
bool rtty_afsk_code(rtty_afsk_t *a, uint8_t code);
/* The end: what is queued now fades out over its last samples */
void rtty_afsk_fade(rtty_afsk_t *a);
/* Up to n samples of what is queued; fewer when the queue runs out */
size_t rtty_afsk_render(rtty_afsk_t *a, int16_t *buf, size_t n);
bool rtty_afsk_busy(const rtty_afsk_t *a);

/* A transmission, its text asked for a character at a time: next gives the next one, -1 when there is none (the end
 * of the transmission: its tail follows). echo is told each character as its codes go into the queue, a few tens
 * of ms before it goes on the air (the audio buffered), with what the far end prints (rtty_enc_shown) */
typedef int  (*rtty_tx_next_fn)(void *ctx);
typedef void (*rtty_tx_echo_fn)(char c, void *ctx);

typedef struct {
    float rate;                 /* samples a second (AUDIO_PLAY_RATE) */
    float baud;
    float mark_hz, space_hz;
} rtty_tx_cfg_t;

typedef struct {
    rtty_afsk_t     afsk;
    rtty_enc_t      enc;
    int             stage;
    float           baud;
    rtty_tx_next_fn next;
    rtty_tx_echo_fn echo;
    void           *ctx;
} rtty_tx_t;

void rtty_tx_start(rtty_tx_t *t, const rtty_tx_cfg_t *cfg, rtty_tx_next_fn next, rtty_tx_echo_fn echo, void *ctx);

/* tx_fill_fn_t (tx_player.h): the next samples of the transmission t, 0 at its end */
size_t rtty_tx_fill(int16_t *buf, size_t n, void *t);

#ifdef __cplusplus
}
#endif
